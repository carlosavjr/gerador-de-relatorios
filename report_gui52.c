/**
 * @file report_gui.c
 * @brief GTK+ application for managing and generating professional PDF reports for professors.
 *
 * Improved version with:
 * - Better memory management, enhanced error handling, thread safety fixes.
 * - Robust file operations, editable category field, dynamic filename generation.
 * - Resolved compilation warnings and linker errors.
 * - Corrected folder structure and UI element sizes.
 * - Updated exiftool tag for category.
 * - Added features: create professor folder, warning dialog for non-existent folders.
 */

#define _GNU_SOURCE

#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <limits.h>
#include <unistd.h>

// =============================================================================
// STRUCTURE DEFINITIONS
// =============================================================================

/**
 * @brief Holds info for each category section in the GUI.
 */
typedef struct {
    GtkWidget *frame;
    GtkWidget *list;
    GtkWidget *add_btn;
    GList *entries;
} CategorySection;

/**
 * @brief Holds info for each PDF entry (row) in the GUI.
 */
typedef struct {
    GtkWidget *box;
    GtkWidget *file_btn;
    GtkWidget *counter_entry;
    GtkWidget *title_entry;
    GtkWidget *year_entry;
    GtkWidget *category_entry;
    GtkWidget *remove_btn;
    GtkWidget *preview_btn;
    gchar *file_path;
    gchar *original_title;
    gchar *original_year;
    gchar *original_category;
    gchar *original_filename_no_ext;
    gboolean has_original_counter;
    gint original_detected_counter;
    gint current_counter_value;
} PdfEntry;

/**
 * @brief Stores data for async exiftool read operations.
 */
typedef struct {
    gchar *file_path;
    GtkListBoxRow *gui_row;
    gint category_index;
    gboolean is_new_file_selection;
    gint stdout_fd;
    gint stderr_fd;
} ExiftoolReadOperation;


// =============================================================================
// GLOBAL VARIABLES
// =============================================================================

gint global_num_categories = 0;
gchar **global_categories = NULL;
CategorySection *global_sections = NULL;
GtkWidget *professor_combo;
GtkWidget *category_nav_list_box;
GtkWidget *category_content_vbox;
GtkWidget *main_category_scrolled_window;
GtkWidget *professors_text_view = NULL;
GtkWidget *categories_text_view = NULL;
GtkWidget *progress_bar = NULL;
GtkWidget *status_label = NULL;
GtkWidget *preview_report_btn = NULL;
GtkWidget *save_all_btn = NULL;
gint *global_max_category_counters = NULL;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

void exiftool_read_completed_callback(GPid pid, gint status, gpointer user_data);
void on_file_set(GtkFileChooserButton *button, GtkListBoxRow *row);
void on_category_entry_changed(GtkEditable *editable, PdfEntry *entry);
void on_counter_entry_changed(GtkEditable *editable, PdfEntry *entry);
gboolean on_professor_selected(GtkComboBox *combo_box, gpointer user_data);
void on_remove_pdf_entry(GtkWidget *button, PdfEntry *entry);
void add_pdf_entry(GtkWidget *widget, CategorySection *section);
void on_preview_pdf(GtkWidget *button, PdfEntry *entry);
void on_preview_report_pdf(GtkWidget *button, gpointer user_data);
void on_save_all_entries(GtkWidget *button, gpointer user_data);
void on_report_generation_finished(GPid pid, gint status, gpointer user_data);
void on_open_category_folder(GtkWidget *button, gint category_index);
void on_reload_professor_button_clicked(GtkWidget *button, gpointer user_data);
void on_category_shortcut_clicked(GtkWidget *button, gpointer user_data);
void on_create_professor_folder_clicked(GtkWidget *button, gpointer user_data);

gboolean load_categories_from_file(const gchar *filepath);
gboolean load_professors_from_file(const gchar *filepath);
void cleanup_category_gui();
void create_category_gui(GtkWidget *nav_list_box, GtkWidget *content_vbox);
void refresh_category_gui();
void refresh_professor_combo();
gchar* sanitize_filename(const gchar *input);
PdfEntry* create_pdf_entry();
GtkListBoxRow* create_pdf_entry_row(PdfEntry *entry, gint category_index);
void add_pdf_entry_to_gui(gint category_index, PdfEntry *entry, GtkListBoxRow *row);
gboolean save_single_pdf_entry(PdfEntry *entry, const gchar *professor_name, gint category_index);
void generate_report(GtkWidget *widget, gpointer data);
void process_pdf_folder(const gchar *folder_path, gint category_index);

void save_professors_list(GtkWidget *button, gpointer user_data);
void save_categories_list(GtkWidget *button, gpointer user_data);
void show_config_dialog(GtkWidget *button, gpointer user_data);
void on_help_button_clicked(GtkWidget *button, gpointer user_data);
void on_help_dialog_close_clicked(GtkWidget *button, GtkWidget *dialog);

void pdf_entry_destroy_notify(gpointer data);
gboolean on_professor_selected_idle_wrapper(gpointer user_data);


// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

#define SAFE_FREE(ptr) do { if (ptr) { g_free(ptr); (ptr) = NULL; } } while (0)

/**
 * @brief Validates a file path for safety.
 */
gboolean is_valid_path(const gchar *path) {
    if (!path) return FALSE;
    if (strlen(path) >= PATH_MAX) {
        g_warning("Path too long: %s", path);
        return FALSE;
    }
    if (strstr(path, "../") != NULL || strstr(path, "/../") != NULL) {
        g_warning("Path contains directory traversal sequence: %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Callback for exiftool completion. Updates GUI with metadata.
 */
void exiftool_read_completed_callback(GPid pid, gint status, gpointer user_data) {
    ExiftoolReadOperation *op = (ExiftoolReadOperation *)user_data;
    if (!op) return;

    g_print("[Main Thread] Exiftool process (PID: %d) finished with status: %d\n", pid, status);

    g_spawn_close_pid(pid);

    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;
    gsize bytes_read;
    gchar buffer[4096];

    GString *stdout_gstring = g_string_new("");
    GString *stderr_gstring = g_string_new("");

    while ((bytes_read = read(op->stdout_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        g_string_append(stdout_gstring, buffer);
    }
    stdout_buf = g_string_free(stdout_gstring, FALSE);

    while ((bytes_read = read(op->stderr_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        g_string_append(stderr_gstring, buffer);
    }
    stderr_buf = g_string_free(stderr_gstring, FALSE);

    close(op->stdout_fd);
    close(op->stderr_fd);

    PdfEntry *entry = g_object_get_data(G_OBJECT(op->gui_row), "pdf-entry");

    if (!entry) {
        g_warning("PdfEntry already freed or GUI row invalid in exiftool_read_completed_callback. Skipping GUI update.");
        SAFE_FREE(stdout_buf);
        SAFE_FREE(stderr_buf);
        SAFE_FREE(op->file_path);
        SAFE_FREE(op);
        return;
    }

    g_print("[Main Thread] Updating GUI for %s\n", op->file_path);

    gchar *extracted_title_local = NULL;
    gchar *extracted_year_local = NULL;
    gchar *extracted_category_local = NULL;
    gchar *original_filename_no_ext_local = NULL;

    gchar *basename = g_path_get_basename(op->file_path);
    gchar *basename_copy = g_strdup(basename);
    gchar *filename_no_ext_ptr = g_strrstr(basename_copy, ".");
    if (filename_no_ext_ptr) {
        *filename_no_ext_ptr = '\0';
    }
    original_filename_no_ext_local = g_strdup(basename_copy);

    if (strlen(basename_copy) >= 3 && g_ascii_isdigit(basename_copy[0]) &&
        g_ascii_isdigit(basename_copy[1]) && basename_copy[2] == '_') {
        gchar counter_str[3];
        counter_str[0] = basename_copy[0];
        counter_str[1] = basename_copy[1];
        counter_str[2] = '\0';
        gint counter_val = atoi(counter_str);

        if (counter_val > 0) {
            entry->has_original_counter = TRUE;
            entry->original_detected_counter = counter_val;
            extracted_title_local = g_strdup(basename_copy + 3);
        }
    }
    if (!extracted_title_local) {
        extracted_title_local = g_strdup(basename_copy);
    }
    SAFE_FREE(basename_copy);
    SAFE_FREE(basename);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && stdout_buf) {
        g_print("  [Main Thread] Exiftool JSON raw output: %s\n", stdout_buf);

        gchar *json_str = stdout_buf;
        gboolean title_found_by_exiftool = FALSE;
        gboolean year_found_by_exiftool = FALSE;
        gboolean category_found_by_exiftool = FALSE;

        gchar *title_key = g_strstr_len(json_str, -1, "\"Title\":");
        if (title_key) {
            title_key += strlen("\"Title\":");
            gchar *title_start = strchr(title_key, '"');
            if (title_start) {
                title_start++;
                gchar *title_end = strchr(title_start, '"');
                if (title_end) {
                    SAFE_FREE(extracted_title_local);
                    extracted_title_local = g_strndup(title_start, title_end - title_start);
                    title_found_by_exiftool = TRUE;
                }
            }
        }

        gchar *year_key = g_strstr_len(json_str, -1, "\"Year\":");
        if (year_key) {
            year_key += strlen("\"Year\":");
            gchar *year_start = strchr(year_key, '"');
            if (year_start) {
                year_start++;
                gchar *year_end = strchr(year_start, '"');
                if (year_end) {
                    extracted_year_local = g_strndup(year_start, year_end - year_start);
                    year_found_by_exiftool = TRUE;
                }
            }
        }

        gchar *category_key = g_strstr_len(json_str, -1, "\"Category\":");
        if (category_key) {
            category_key += strlen("\"Category\":");
            gchar *category_start = strchr(category_key, '"');
            if (category_start) {
                category_start++;
                gchar *category_end = strchr(category_start, '"');
                if (category_end) {
                    extracted_category_local = g_strndup(category_start, category_end - category_start);
                    category_found_by_exiftool = TRUE;
                }
            }
        }

        if (title_found_by_exiftool) {
            g_print("  [Main Thread] Exiftool found title: \"%s\"\n", extracted_title_local);
        } else {
            g_print("  [Main Thread] Exiftool did NOT find a title. Using filename-derived title: \"%s\"\n", extracted_title_local);
        }
        if (year_found_by_exiftool) {
            g_print("  [Main Thread] Exiftool found year: \"%s\"\n", extracted_year_local);
        } else {
            g_print("  [Main Thread] Exiftool did NOT find a year. Using filename-derived year (if any): \"%s\"\n", extracted_year_local ? extracted_year_local : "(null)");
        }
        if (category_found_by_exiftool) {
            g_print("  [Main Thread] Exiftool found category: \"%s\"\n", extracted_category_local);
        } else {
            g_print("  [Main Thread] Exiftool did NOT find a category.\n");
        }
    } else {
        g_warning("  [Main Thread] Exiftool command failed for %s. Exit status: %d. Stderr: %s",
                  op->file_path, WEXITSTATUS(status), stderr_buf ? stderr_buf : "(empty)");
    }

    if (!extracted_year_local) {
        gchar *temp_filename_for_year_parsing = g_path_get_basename(op->file_path);
        gchar *filename_no_ext_temp = g_strrstr(temp_filename_for_year_parsing, ".");
        if (filename_no_ext_temp) {
            *filename_no_ext_temp = '\0';
        }
        gchar *last_underscore = g_strrstr(temp_filename_for_year_parsing, "_");
        if (last_underscore && strlen(last_underscore + 1) == 4) {
            gboolean is_numeric = TRUE;
            for (gint k = 0; k < 4; k++) {
                if (!g_ascii_isdigit(last_underscore[1 + k])) {
                    is_numeric = FALSE;
                    break;
                }
            }
            if (is_numeric) {
                extracted_year_local = g_strdup(last_underscore + 1);
            }
        }
        SAFE_FREE(temp_filename_for_year_parsing);
    }

    if (!extracted_category_local && op->category_index >= 0 && op->category_index < global_num_categories) {
        extracted_category_local = g_strdup(global_categories[op->category_index]);
    } else if (!extracted_category_local) {
        extracted_category_local = g_strdup("documento");
    }

    gtk_entry_set_text(GTK_ENTRY(entry->title_entry), extracted_title_local ? extracted_title_local : "Título Não Informado");
    gtk_entry_set_text(GTK_ENTRY(entry->year_entry), extracted_year_local ? extracted_year_local : "");
    gtk_entry_set_text(GTK_ENTRY(entry->category_entry), extracted_category_local);

    SAFE_FREE(entry->original_title);
    entry->original_title = g_strdup(extracted_title_local ? extracted_title_local : "Título Não Informado");
    SAFE_FREE(entry->original_year);
    entry->original_year = g_strdup(extracted_year_local ? extracted_year_local : "");
    SAFE_FREE(entry->original_category);
    entry->original_category = g_strdup(extracted_category_local);
    SAFE_FREE(entry->original_filename_no_ext);
    entry->original_filename_no_ext = original_filename_no_ext_local;
    entry->current_counter_value = entry->original_detected_counter;

    if (!op->is_new_file_selection && entry->original_detected_counter > 0) {
        if (entry->original_detected_counter > global_max_category_counters[op->category_index]) {
            global_max_category_counters[op->category_index] = entry->original_detected_counter;
        }
    }
    gchar *counter_text = g_strdup_printf("%02d", entry->original_detected_counter);
    gtk_entry_set_text(GTK_ENTRY(entry->counter_entry), counter_text);
    SAFE_FREE(counter_text);

    GtkWidget *list_box = gtk_widget_get_parent(GTK_WIDGET(op->gui_row));
    if (list_box && GTK_IS_LIST_BOX(list_box)) {
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(list_box));
    }

    gdouble current_fraction = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(progress_bar));
    if (current_fraction < 0.2) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), current_fraction + (0.2 / (global_num_categories * 10.0)));
        gchar *progress_text = g_strdup_printf("%d%%", (gint)(current_fraction * 100));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text);
        SAFE_FREE(progress_text);
        g_main_context_iteration(NULL, FALSE);
    }
    gboolean all_pdfs_processed = TRUE;
    for (gint i = 0; i < global_num_categories; i++) {
        GList *l;
        for (l = global_sections[i].entries; l != NULL; l = g_list_next(l)) {
            PdfEntry *current_entry = (PdfEntry *)l->data;
            if (g_strcmp0(gtk_entry_get_text(GTK_ENTRY(current_entry->title_entry)), "Carregando título...") == 0) {
                all_pdfs_processed = FALSE;
                break;
            }
        }
        if (!all_pdfs_processed) break;
    }
    if (all_pdfs_processed) {
        gtk_label_set_text(GTK_LABEL(status_label), "Carregamento concluído. Pronto para gerar relatório.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.2);
        gchar *progress_text = g_strdup_printf("%d%%", (gint)(0.2 * 100));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text);
        SAFE_FREE(progress_text);
        g_main_context_iteration(NULL, FALSE);
        const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));
        if (professor) {
            gchar *sanitized_professor_name = sanitize_filename(professor);
            gchar *pdf_filename = g_strdup_printf("final/%s_relatorio_profissional.pdf", sanitized_professor_name);
            if (g_file_test(pdf_filename, G_FILE_TEST_EXISTS)) {
                if (preview_report_btn) {
                    gtk_widget_show(preview_report_btn);
                }
            } else {
                if (preview_report_btn) {
                    gtk_widget_hide(preview_report_btn);
                }
            }
            SAFE_FREE(pdf_filename);
            SAFE_FREE(sanitized_professor_name);
        }
    }
    SAFE_FREE(extracted_title_local);
    SAFE_FREE(extracted_year_local);
    SAFE_FREE(extracted_category_local);
    SAFE_FREE(stdout_buf);
    SAFE_FREE(stderr_buf);
    SAFE_FREE(op->file_path);
    SAFE_FREE(op);
}

/**
 * @brief Callback for file selection. Updates entry and triggers async exiftool.
 */
void on_file_set(GtkFileChooserButton *button, GtkListBoxRow *row) {
    g_print("File set callback triggered.\n");

    PdfEntry *entry = g_object_get_data(G_OBJECT(row), "pdf-entry");
    if (!entry) {
        g_warning("PdfEntry data not found for the row. Cannot process file selection.");
        return;
    }

    gchar *file_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(button));
    if (!file_path) {
        g_warning("No file path selected. Skipping processing.");
        return;
    }

    if (!is_valid_path(file_path)) {
        g_warning("Selected file path is invalid: %s", file_path);
        SAFE_FREE(file_path);
        return;
    }

    g_print("  Selected file: %s\n", file_path);

    SAFE_FREE(entry->file_path);
    entry->file_path = g_strdup(file_path);
    SAFE_FREE(file_path);

    gtk_entry_set_text(GTK_ENTRY(entry->title_entry), "Carregando título...");
    gtk_entry_set_text(GTK_ENTRY(entry->year_entry), "Carregando ano...");
    gtk_entry_set_text(GTK_ENTRY(entry->category_entry), "Carregando categoria...");

    gint category_index = -1;
    GtkWidget *list_box = gtk_widget_get_parent(GTK_WIDGET(row));
    if (list_box) {
        for (gint i = 0; i < global_num_categories; i++) {
            if (global_sections[i].list == list_box) {
                category_index = i;
                break;
            }
        }
    }
    if (category_index == -1) {
        g_warning("Could not determine category index for the selected PDF. Exiftool processing might be affected.");
    }

    ExiftoolReadOperation *op = g_malloc0(sizeof(ExiftoolReadOperation));
    op->file_path = g_strdup(entry->file_path);
    op->gui_row = row;
    op->category_index = category_index;
    op->is_new_file_selection = TRUE;

    gchar *quoted_file_path = g_shell_quote(op->file_path);
    gchar *command_line = g_strdup_printf("exiftool -j -Title -Year -Category %s", quoted_file_path);
    SAFE_FREE(quoted_file_path);

    gchar **argv = NULL;
    gint argc = 0;
    GError *spawn_error = NULL;
    gboolean success = g_shell_parse_argv(command_line, &argc, &argv, &spawn_error);
    if (!success) {
        g_warning("Failed to parse exiftool command line for new file: %s (%s)", command_line, spawn_error ? spawn_error->message : "N/A");
        if (spawn_error) g_error_free(spawn_error);
        SAFE_FREE(command_line);
        SAFE_FREE(op->file_path);
        SAFE_FREE(op);
        return;
    }

    GPid pid;
    gint stdout_fd_local, stderr_fd_local;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &pid, NULL, &stdout_fd_local, &stderr_fd_local, &spawn_error)) {
        g_warning("Failed to spawn exiftool process for new file: %s", spawn_error ? spawn_error->message : "N/A");
        if (spawn_error) g_error_free(spawn_error);
        SAFE_FREE(op->file_path);
        SAFE_FREE(op);
        g_strfreev(argv);
        SAFE_FREE(command_line);
        return;
    }

    op->stdout_fd = stdout_fd_local;
    op->stderr_fd = stderr_fd_local;

    g_strfreev(argv);
    SAFE_FREE(command_line);

    g_child_watch_add(pid, exiftool_read_completed_callback, op);
}

/**
 * @brief Callback for counter entry changes. Updates internal value and re-sorts list.
 */
void on_counter_entry_changed(GtkEditable *editable, PdfEntry *entry) {
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(editable));
    gint new_val = atoi(text);
    if (new_val < 0) new_val = 0;

    if (entry->current_counter_value != new_val) {
        entry->current_counter_value = new_val;

        GtkWidget *parent_row = gtk_widget_get_parent(entry->box);
        if (parent_row && GTK_IS_LIST_BOX_ROW(parent_row)) {
            GtkWidget *list_box = gtk_widget_get_parent(parent_row);
            if (list_box && GTK_IS_LIST_BOX(list_box)) {
                gtk_list_box_invalidate_sort(GTK_LIST_BOX(list_box));
            }
        }
    }
}

/**
 * @brief Callback for category entry changes. Updates internal category string.
 */
void on_category_entry_changed(GtkEditable *editable, PdfEntry *entry) {
    const gchar *new_category = gtk_entry_get_text(GTK_ENTRY(editable));
    g_print("Category changed for %s to: %s\n", entry->file_path ? entry->file_path : "N/A", new_category);
    SAFE_FREE(entry->original_category);
    entry->original_category = g_strdup(new_category);
}

/**
 * @brief Compares PdfEntries by their current_counter_value for sorting.
 */
gint compare_pdf_entries_by_counter(GtkListBoxRow *row1, GtkListBoxRow *row2, gpointer user_data) {
    (void)user_data;

    PdfEntry *entry_a = g_object_get_data(G_OBJECT(row1), "pdf-entry");
    PdfEntry *entry_b = g_object_get_data(G_OBJECT(row2), "pdf-entry");

    if (!entry_a || !entry_b) return 0;

    gint counter_a = entry_a->current_counter_value;
    gint counter_b = entry_b->current_counter_value;

    return counter_a - counter_b;
}

/**
 * @brief Frees dynamically allocated memory within a PdfEntry struct.
 */
void pdf_entry_destroy_notify(gpointer data) {
    PdfEntry *entry = (PdfEntry *)data;
    if (entry) {
        g_print("Freeing PdfEntry data for: %s (GDestroyNotify)\n", entry->file_path ? entry->file_path : "N/A");
        SAFE_FREE(entry->file_path);
        SAFE_FREE(entry->original_title);
        SAFE_FREE(entry->original_year);
        SAFE_FREE(entry->original_category);
        SAFE_FREE(entry->original_filename_no_ext);
        SAFE_FREE(entry);
    }
}

/**
 * @brief Sanitizes a string for use as a filename.
 */
gchar* sanitize_filename(const gchar *input) {
    if (!input) return g_strdup("documento_sem_titulo");

    GString *clean_string = g_string_new("");
    const gchar *p = input;
    gunichar c;

    while (*p) {
        c = g_utf8_get_char(p);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '\"' || c == '<' || c == '>' || c == '|' || c == '\0') {
            g_string_append_unichar(clean_string, '_');
        } else if (g_unichar_isspace(c)) {
            g_string_append_unichar(clean_string, '_');
        } else {
            g_string_append_unichar(clean_string, c);
        }
        p = g_utf8_next_char(p);
    }

    gchar *temp_sanitized = g_string_free(clean_string, FALSE);

    GString *final_sanitized = g_string_new("");
    gboolean last_was_underscore = FALSE;
    const gchar *current_char_ptr = temp_sanitized;

    while (*current_char_ptr) {
        if (*current_char_ptr == '_') {
            if (!last_was_underscore) {
                g_string_append_unichar(final_sanitized, '_');
            }
            last_was_underscore = TRUE;
        } else {
            g_string_append_unichar(final_sanitized, g_utf8_get_char(current_char_ptr));
            last_was_underscore = FALSE;
        }
        current_char_ptr = g_utf8_next_char(current_char_ptr);
    }

    SAFE_FREE(temp_sanitized);

    gchar *result = g_string_free(final_sanitized, FALSE);
    gchar *trimmed_result = g_strstrip(result);

    if (g_strcmp0(trimmed_result, "") == 0) {
        SAFE_FREE(result);
        return g_strdup("documento_sem_titulo");
    }

    return result;
}

/**
 * @brief Creates and initializes a new PdfEntry struct.
 */
PdfEntry* create_pdf_entry() {
    PdfEntry *entry = g_new0(PdfEntry, 1);
    entry->file_path = NULL;
    entry->original_title = NULL;
    entry->original_year = NULL;
    entry->original_category = NULL;
    entry->original_filename_no_ext = NULL;
    entry->has_original_counter = FALSE;
    entry->original_detected_counter = 0;
    entry->current_counter_value = 0;
    return entry;
}

/**
 * @brief Creates a GtkListBoxRow with widgets for a PdfEntry.
 */
GtkListBoxRow* create_pdf_entry_row(PdfEntry *entry, gint category_index) {
    (void)category_index;

    GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
    entry->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    g_object_set_data_full(G_OBJECT(row), "pdf-entry", entry, pdf_entry_destroy_notify);

    entry->file_btn = gtk_file_chooser_button_new("Selecionar PDF", GTK_FILE_CHOOSER_ACTION_OPEN);
    gtk_widget_set_size_request(entry->file_btn, 150, -1);
    gtk_box_pack_start(GTK_BOX(entry->box), entry->file_btn, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(entry->file_btn), "file-set", G_CALLBACK(on_file_set), row);

    entry->counter_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry->counter_entry), 3);
    gtk_entry_set_max_length(GTK_ENTRY(entry->counter_entry), 2);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->counter_entry), "NN");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->counter_entry, FALSE, FALSE, 5);
    g_signal_connect(G_OBJECT(entry->counter_entry), "changed", G_CALLBACK(on_counter_entry_changed), entry);

    entry->title_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->title_entry), "Título do PDF");
    gtk_widget_set_hexpand(entry->title_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(entry->box), entry->title_entry, TRUE, TRUE, 0);

    entry->year_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry->year_entry), 5);
    gtk_entry_set_max_length(GTK_ENTRY(entry->year_entry), 4);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->year_entry), "Ano");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->year_entry, FALSE, FALSE, 5);

    entry->category_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry->category_entry), 15);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->category_entry), "Categoria (ex: artigo)");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->category_entry, FALSE, FALSE, 5);
    g_signal_connect(G_OBJECT(entry->category_entry), "changed", G_CALLBACK(on_category_entry_changed), entry);

    entry->preview_btn = gtk_button_new_with_label("Visualizar");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->preview_btn, FALSE, FALSE, 5);
    g_signal_connect(G_OBJECT(entry->preview_btn), "clicked", G_CALLBACK(on_preview_pdf), entry);

    entry->remove_btn = gtk_button_new_with_label("Remover");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->remove_btn, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(entry->remove_btn), "clicked", G_CALLBACK(on_remove_pdf_entry), entry);

    gtk_container_add(GTK_CONTAINER(row), entry->box);
    gtk_widget_show_all(entry->box);

    return row;
}

/**
 * @brief Adds a PdfEntry to the GUI and its category's entry list.
 */
void add_pdf_entry_to_gui(gint category_index, PdfEntry *entry, GtkListBoxRow *row) {
    if (category_index >= 0 && category_index < global_num_categories) {
        gtk_list_box_insert(GTK_LIST_BOX(global_sections[category_index].list), GTK_WIDGET(row), -1);
        global_sections[category_index].entries = g_list_append(global_sections[category_index].entries, entry);
        gtk_widget_show_all(GTK_WIDGET(row));
    } else {
        g_warning("Invalid category index %d for adding PDF entry to GUI.", category_index);
    }
}

/**
 * @brief Loads categories from 'config/categories.txt'.
 */
gboolean load_categories_from_file(const gchar *filepath) {
    if (global_categories) {
        for (gint i = 0; i < global_num_categories; i++) {
            SAFE_FREE(global_categories[i]);
        }
        SAFE_FREE(global_categories);
    }
    global_num_categories = 0;

    GList *category_list = NULL;
    gchar *line = NULL;
    gsize len = 0;
    gssize read;
    FILE *fp = fopen(filepath, "r");

    if (!fp) {
        g_warning("Could not open categories file '%s': %s", filepath, g_strerror(errno));
        return FALSE;
    }

    while ((read = getline(&line, &len, fp)) != -1) {
        line[strcspn(line, "\n")] = 0;
        if (g_strcmp0(line, "") != 0) {
            category_list = g_list_append(category_list, g_strdup(line));
        }
    }
    fclose(fp);
    SAFE_FREE(line);

    global_num_categories = g_list_length(category_list);
    if (global_num_categories == 0) {
        g_warning("No categories found in '%s'. Please add categories, one per line.", filepath);
        g_list_free_full(category_list, g_free);
        return FALSE;
    }

    global_categories = g_new0(gchar*, global_num_categories + 1);
    gint i = 0;
    for (GList *l = category_list; l != NULL; l = l->next) {
        global_categories[i++] = (gchar*)l->data;
    }
    global_categories[i] = NULL;

    g_list_free(category_list);

    g_print("Loaded %d categories from '%s'.\n", global_num_categories, filepath);
    return TRUE;
}

/**
 * @brief Loads professor names from 'config/professores.txt'.
 */
gboolean load_professors_from_file(const gchar *filepath) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(professor_combo));

    FILE *fp = fopen(filepath, "r");
    if(fp) {
        char line[256];
        while(fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = 0;
            if (g_strcmp0(line, "") != 0) {
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(professor_combo), line);
            }
        }
        fclose(fp);
        return TRUE;
    } else {
        g_warning("Could not open 'config/professores.txt'. Please ensure the file exists.");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(professor_combo), "Nenhum Professor Disponível");
        return FALSE;
    }
}

/**
 * @brief Saves professor list from config dialog and creates directories.
 */
void save_professors_list(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(professors_text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gchar *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    GError *error = NULL;
    if (g_file_set_contents("config/professores.txt", content, -1, &error)) {
        g_print("Professors list saved. Now creating directories...\n");
        gchar *professor_base_dir = g_strdup("./");

        gchar **professors_array = g_strsplit(content, "\n", 0);
        for (gint p_idx = 0; professors_array[p_idx] != NULL; p_idx++) {
            gchar *professor_name = g_strstrip(professors_array[p_idx]);
            if (g_strcmp0(professor_name, "") == 0) continue;

            g_print("  Processing professor: %s\n", professor_name);

            gchar *professor_folder_path = g_strdup_printf("%s%s", professor_base_dir, professor_name);
            if (!g_mkdir_with_parents(professor_folder_path, 0755)) {
                if (errno != EEXIST) {
                    g_warning("    Failed to create professor directory '%s': %s", professor_folder_path, g_strerror(errno));
                } else {
                    g_print("    Professor directory already exists: %s\n", professor_folder_path);
                }
            } else {
                g_print("    Professor directory created: %s\n", professor_folder_path);
            }

            for (gint c_idx = 0; c_idx < global_num_categories; c_idx++) {
                if (c_idx < 0 || c_idx >= global_num_categories || !global_categories[c_idx]) {
                    g_warning("Invalid category index or NULL category name during directory creation for professor '%s'. Skipping.", professor_name);
                    continue;
                }
                gchar *category_folder_path = g_strdup_printf("%s/%s", professor_folder_path, global_categories[c_idx]);
                if (!g_mkdir_with_parents(category_folder_path, 0755)) {
                    if (errno != EEXIST) {
                        g_warning("      Failed to create category directory '%s' for '%s': %s",
                                  global_categories[c_idx], professor_name, g_strerror(errno));
                    } else {
                        g_print("      Category directory already exists: %s\n", category_folder_path);
                    }
                } else {
                    g_print("      Category directory created: %s\n", category_folder_path);
                }
                SAFE_FREE(category_folder_path);
            }
            SAFE_FREE(professor_folder_path);
        }
        g_strfreev(professors_array);
        SAFE_FREE(professor_base_dir);

        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Lista de professores salva com sucesso!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        refresh_professor_combo();
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Erro ao salvar lista de professores: %s", error ? error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error);
    }
    SAFE_FREE(content);
}

/**
 * @brief Saves category list from config dialog.
 */
void save_categories_list(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(categories_text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gchar *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    GError *error = NULL;
    if (g_file_set_contents("config/categories.txt", content, -1, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Lista de categorias salva com sucesso!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        refresh_category_gui();
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Erro ao salvar lista de categorias: %s", error ? error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error);
    }
    SAFE_FREE(content);
}

// =============================================================================
// GUI MANAGEMENT FUNCTIONS
// =============================================================================

/**
 * @brief Cleans up all existing category GUI elements.
 */
void cleanup_category_gui() {
    if (global_sections) {
        for (gint i = 0; i < global_num_categories; i++) {
            if (global_sections[i].frame) {
                gtk_widget_destroy(global_sections[i].frame);
            }
            if (global_sections[i].entries) {
                g_list_free(global_sections[i].entries);
                global_sections[i].entries = NULL;
            }
        }
        SAFE_FREE(global_sections);
    }

    if (category_content_vbox) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(category_content_vbox));
        for (GList *l = children; l != NULL; l = g_list_next(l)) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children);
    }

    if (category_nav_list_box) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(category_nav_list_box));
        for (GList *l = children; l != NULL; l = l->next) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children);
    }
}

/**
 * @brief Creates and populates category GUI elements.
 */
void create_category_gui(GtkWidget *nav_list_box, GtkWidget *content_vbox) {
    global_sections = g_new0(CategorySection, global_num_categories);

    for(gint i = 0; i < global_num_categories; i++) {
        global_sections[i].frame = gtk_frame_new(global_categories[i]);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

        global_sections[i].list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(global_sections[i].list), GTK_SELECTION_NONE);
        gtk_widget_set_hexpand(GTK_WIDGET(global_sections[i].list), TRUE);
        gtk_widget_set_halign(GTK_WIDGET(global_sections[i].list), GTK_ALIGN_FILL);

        GtkWidget *buttons_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_widget_set_halign(buttons_hbox, GTK_ALIGN_END);

        global_sections[i].add_btn = gtk_button_new_with_label("Adicionar PDF");
        g_signal_connect(global_sections[i].add_btn, "clicked", G_CALLBACK(add_pdf_entry), &global_sections[i]);
        gtk_box_pack_end(GTK_BOX(buttons_hbox), global_sections[i].add_btn, FALSE, FALSE, 0);

        GtkWidget *open_folder_btn = gtk_button_new_with_label("Abrir Pasta");
        g_signal_connect(open_folder_btn, "clicked", G_CALLBACK(on_open_category_folder), GINT_TO_POINTER(i));
        gtk_box_pack_end(GTK_BOX(buttons_hbox), open_folder_btn, FALSE, FALSE, 0);

        GtkWidget *list_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scrolled_window),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_hexpand(list_scrolled_window, TRUE);
        gtk_widget_set_halign(list_scrolled_window, GTK_ALIGN_FILL);
        gtk_widget_set_size_request(list_scrolled_window, -1, 300);
        gtk_container_add(GTK_CONTAINER(list_scrolled_window), global_sections[i].list);

        gtk_box_pack_start(GTK_BOX(box), list_scrolled_window, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), buttons_hbox, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(global_sections[i].frame), box);

        gtk_box_pack_start(GTK_BOX(content_vbox), global_sections[i].frame, TRUE, TRUE, 30);
        gtk_widget_set_hexpand(GTK_WIDGET(global_sections[i].frame), TRUE);
        gtk_widget_set_halign(GTK_WIDGET(global_sections[i].frame), GTK_ALIGN_FILL);
        gtk_widget_set_vexpand(GTK_WIDGET(global_sections[i].frame), TRUE);

        GtkWidget *shortcut_btn = gtk_button_new_with_label(global_categories[i]);
        g_object_set_data(G_OBJECT(shortcut_btn), "category-frame", global_sections[i].frame);
        g_signal_connect(shortcut_btn, "clicked", G_CALLBACK(on_category_shortcut_clicked), NULL);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), shortcut_btn);
        gtk_list_box_insert(GTK_LIST_BOX(nav_list_box), GTK_WIDGET(row), -1);
    }
    gtk_widget_show_all(nav_list_box);
    gtk_widget_show_all(content_vbox);
}

/**
 * @brief Refreshes the category GUI after config changes.
 */
void refresh_category_gui() {
    g_print("Refreshing category GUI...\n");
    cleanup_category_gui();
    if (load_categories_from_file("config/categories.txt")) {
        create_category_gui(category_nav_list_box, category_content_vbox);
    } else {
        g_warning("Failed to refresh categories after loading from file.");
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Refreshes the professor combo box after config changes.
 */
void refresh_professor_combo() {
    g_print("Refreshing professor combo...\n");
    if (!load_professors_from_file("config/professores.txt")) {
        g_warning("Failed to refresh professors after loading from file.");
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Adds a new, blank PdfEntry to the GUI.
 */
void add_pdf_entry(GtkWidget *widget, CategorySection *section) {
    (void)widget;

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));

    if (!professor || g_strcmp0(professor, "") == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Por favor, selecione um professor antes de adicionar um PDF.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    PdfEntry *entry = create_pdf_entry();
    entry->has_original_counter = FALSE;
    entry->original_detected_counter = 0;
    entry->current_counter_value = 0;
    entry->original_filename_no_ext = g_strdup("");
    entry->original_category = g_strdup(global_categories[section - global_sections]);

    GtkListBoxRow *row = create_pdf_entry_row(entry, section - global_sections);

    add_pdf_entry_to_gui(section - global_sections, entry, row);
}

/**
 * @brief Removes a PdfEntry from GUI and moves its file to 'old_files'.
 */
void on_remove_pdf_entry(GtkWidget *button, PdfEntry *entry) {
    (void)button;

    g_print("Removing PDF entry: %s\n", entry->file_path ? entry->file_path : "N/A");

    GtkWidget *row = NULL;
    GtkWidget *list_box = NULL;
    CategorySection *current_section = NULL;
    gint category_index = -1;
    const gchar *professor = NULL;

    row = gtk_widget_get_parent(entry->box);
    if (row && GTK_IS_LIST_BOX_ROW(row)) {
        list_box = gtk_widget_get_parent(row);
        if (list_box && GTK_IS_LIST_BOX(list_box)) {
            for (gint i = 0; i < global_num_categories; i++) {
                if (global_sections[i].list == list_box) {
                    current_section = &global_sections[i];
                    category_index = i;
                    break;
                }
            }
        }
    }

    professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));

    if (professor && entry->file_path && is_valid_path(entry->file_path) && g_file_test(entry->file_path, G_FILE_TEST_EXISTS) && current_section) {
        gchar *professor_base_dir = g_strdup("./");
        gchar *dest_category_dir = g_strdup_printf("%s%s/%s", professor_base_dir, professor, global_categories[category_index]);
        gchar *old_files_dir = g_strdup_printf("%s/old_files", dest_category_dir);

        g_print("  Attempting to ensure old_files directory for removal exists: %s\n", old_files_dir);
        if (!g_mkdir_with_parents(old_files_dir, 0755)) {
            if (errno != EEXIST) {
                g_warning("Failed to create old_files directory for removal: %s (%s)", old_files_dir, g_strerror(errno));
            } else {
                g_print("  Old_files directory for removal already exists: %s\n", old_files_dir);
            }
        } else {
            g_print("  Old_files directory for removal created: %s\n", old_files_dir);
        }

        gchar *original_basename = g_path_get_basename(entry->file_path);
        gchar *old_file_dest_path = g_strdup_printf("%s/%s", old_files_dir, original_basename);

        g_print("  Moving original file from %s to %s due to 'Remover' button click.\n", entry->file_path, old_file_dest_path);

        GError *move_error = NULL;
        GFile *original_gfile_to_move = g_file_new_for_path(entry->file_path);
        GFile *old_dest_gfile = g_file_new_for_path(old_file_dest_path);

        if (!g_file_move(original_gfile_to_move, old_dest_gfile, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &move_error)) {
            g_warning("Failed to move original file from %s to %s on 'Remover' click: %s",
                      entry->file_path, old_file_dest_path, move_error ? move_error->message : "N/A");
            if (move_error) g_error_free(move_error);
        } else {
            g_print("  Original file successfully moved to old_files by 'Remover' button.\n");
        }

        if (original_gfile_to_move) g_object_unref(original_gfile_to_move);
        if (old_dest_gfile) g_object_unref(old_dest_gfile);
        SAFE_FREE(original_basename);
        SAFE_FREE(old_file_dest_path);
        SAFE_FREE(old_files_dir);
        SAFE_FREE(dest_category_dir);
        SAFE_FREE(professor_base_dir);
    } else {
        if (!professor) {
            g_warning("No professor selected. Cannot determine 'old_files' destination for removal. Skipping file move.");
        } else if (!entry->file_path || !g_file_test(entry->file_path, G_FILE_TEST_EXISTS)) {
            g_print("File %s does not exist or file_path is NULL. Skipping file move to old_files.\n", entry->file_path ? entry->file_path : "N/A");
        } else if (!current_section) {
            g_warning("Could not find parent CategorySection for the listbox. Skipping file move.");
        }
    }

    if (current_section) {
        current_section->entries = g_list_remove(current_section->entries, entry);
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(current_section->list));
    }

    if (row) {
        gtk_widget_destroy(GTK_WIDGET(row));
        g_print("PDF entry removed from GUI. PdfEntry struct will be freed by GDestroyNotify.\n");
    } else {
        g_warning("GUI context (row) invalid for full GUI removal. PdfEntry struct will not be automatically freed by GTK.");
    }
}

/**
 * @brief Callback for professor selection. Clears old entries, loads new.
 */
gboolean on_professor_selected(GtkComboBox *combo_box, gpointer user_data) {
    (void)user_data;

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_box));

    if (preview_report_btn) {
        gtk_widget_hide(preview_report_btn);
    }

    gtk_label_set_text(GTK_LABEL(status_label), "Carregando informações do professor...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gchar *progress_text_0 = g_strdup_printf("%d%%", 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0);
    SAFE_FREE(progress_text_0);
    g_main_context_iteration(NULL, FALSE);

    for (gint i = 0; i < global_num_categories; i++) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(global_sections[i].list));
        for (GList *l = children; l != NULL; l = g_list_next(l)) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children);

        if (global_sections[i].entries) {
            g_list_free(global_sections[i].entries);
            global_sections[i].entries = NULL;
        }
    }

    SAFE_FREE(global_max_category_counters);
    global_max_category_counters = g_new0(gint, global_num_categories);

    if (!professor) {
        g_print("No professor selected or selection cleared. All entries cleared.\n");
        gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_0_again = g_strdup_printf("%d%%", 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0_again);
        SAFE_FREE(progress_text_0_again);
        g_main_context_iteration(NULL, FALSE);
        return G_SOURCE_REMOVE;
    }

    gchar *exiftool_path = g_find_program_in_path("exiftool");
    if (!exiftool_path) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Erro: O programa 'exiftool' não foi encontrado no seu PATH.\n"
                                                   "Por favor, instale-o (sudo apt install libimage-exiftool-perl) e tente novamente.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        gtk_label_set_text(GTK_LABEL(status_label), "Erro: exiftool não encontrado.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error);
        g_main_context_iteration(NULL, FALSE);
        SAFE_FREE(exiftool_path);
        return G_SOURCE_REMOVE;
    }
    SAFE_FREE(exiftool_path);

    g_print("Professor selected: %s. Initiating folder scan for all categories.\n", professor);

    for (gint i = 0; i < global_num_categories; i++) {
        gchar *professor_base_dir = g_strdup("./");
        gchar *category_folder_path = g_strdup_printf("%s%s/%s", professor_base_dir, professor, global_categories[i]);

        process_pdf_folder(category_folder_path, i);

        gtk_list_box_set_sort_func(GTK_LIST_BOX(global_sections[i].list), (GtkListBoxSortFunc)compare_pdf_entries_by_counter, NULL, NULL);
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(global_sections[i].list));

        SAFE_FREE(category_folder_path);
        SAFE_FREE(professor_base_dir);
    }

    gtk_label_set_text(GTK_LABEL(status_label), "Carregamento iniciado. Aguardando metadados...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.05);
    gchar *progress_text_5 = g_strdup_printf("%d%%", 5);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_5);
    SAFE_FREE(progress_text_5);
    g_main_context_iteration(NULL, FALSE);

    return G_SOURCE_REMOVE;
}

/**
 * @brief Opens the selected category folder in the system file manager.
 */
void on_open_category_folder(GtkWidget *button, gint category_index) {
    (void)button;

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));
    if (!professor || g_strcmp0(professor, "") == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Por favor, selecione um professor para abrir a pasta da categoria.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (category_index < 0 || category_index >= global_num_categories || !global_categories[category_index]) {
        g_warning("Invalid category index provided to on_open_category_folder: %d", category_index);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Erro interno: Categoria inválida para abrir a pasta.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    gchar *professor_base_dir = g_strdup("./");
    gchar *category_folder_path = g_strdup_printf("%s%s/%s", professor_base_dir, professor, global_categories[category_index]);

    g_print("Attempting to open category folder: %s\n", category_folder_path);

    if (!is_valid_path(category_folder_path)) {
        g_warning("Invalid category folder path: %s", category_folder_path);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Caminho inválido para a pasta da categoria: %s", category_folder_path);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        SAFE_FREE(category_folder_path);
        SAFE_FREE(professor_base_dir);
        return;
    }


    if (!g_file_test(category_folder_path, G_FILE_TEST_IS_DIR)) {
        g_warning("Category folder does not exist: %s", category_folder_path);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_WARNING,
                                                   GTK_BUTTONS_OK,
                                                   "A pasta para a categoria '%s' do professor '%s' não existe.\n"
                                                   "Por favor, use o botão 'Criar Pasta do Professor' para criá-la.",
                                                   global_categories[category_index], professor);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        SAFE_FREE(category_folder_path);
        SAFE_FREE(professor_base_dir);
        return;
    }

    GError *spawn_error = NULL;
    gchar *command_line = g_strdup_printf("xdg-open \"%s\"", category_folder_path);

    if (!g_spawn_command_line_async(command_line, &spawn_error)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Falha ao abrir a pasta: %s", spawn_error ? spawn_error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (spawn_error) g_error_free(spawn_error);
    } else {
        g_print("Category folder opened successfully.\n");
    }

    SAFE_FREE(command_line);
    SAFE_FREE(category_folder_path);
    SAFE_FREE(professor_base_dir);
}

/**
 * @brief Scans a folder for PDF files, creates PdfEntries, and adds to GUI.
 */
void process_pdf_folder(const gchar *folder_path, gint category_index) {
    DIR *d;
    struct dirent *dir;
    g_print("Starting PDF folder scan in: %s for category index %d\n", folder_path, category_index);

    if (!is_valid_path(folder_path)) {
        g_warning("Invalid folder path provided to process_pdf_folder: %s", folder_path);
        return;
    }

    d = opendir(folder_path);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (g_strcmp0(dir->d_name, ".") == 0 || g_strcmp0(dir->d_name, "..") == 0) {
                continue;
            }

            gchar *full_path = g_build_filename(folder_path, dir->d_name, NULL);
            if (!is_valid_path(full_path)) {
                g_warning("Skipping invalid file path: %s", full_path);
                SAFE_FREE(full_path);
                continue;
            }

            GFile *file = g_file_new_for_path(full_path);
            GError *file_info_error = NULL;
            GFileInfo *file_info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL, &file_info_error);

            if (file_info && g_file_info_get_file_type(file_info) == G_FILE_TYPE_REGULAR) {
                gchar *lower_case_filename = g_ascii_strdown(dir->d_name, -1);
                if (g_str_has_suffix(lower_case_filename, ".pdf")) {
                    g_print("  Found PDF: %s\n", full_path);

                    PdfEntry *new_entry = create_pdf_entry();
                    new_entry->file_path = g_strdup(full_path);

                    GtkListBoxRow *row = create_pdf_entry_row(new_entry, category_index);
                    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(new_entry->file_btn), full_path);

                    add_pdf_entry_to_gui(category_index, new_entry, row);
                    gtk_entry_set_text(GTK_ENTRY(new_entry->title_entry), "Carregando título...");
                    gtk_entry_set_text(GTK_ENTRY(new_entry->year_entry), "Carregando ano...");
                    gtk_entry_set_text(GTK_ENTRY(new_entry->category_entry), "Carregando categoria...");

                    ExiftoolReadOperation *op = g_malloc0(sizeof(ExiftoolReadOperation));
                    op->file_path = g_strdup(full_path);
                    op->gui_row = row;
                    op->category_index = category_index;
                    op->is_new_file_selection = FALSE;

                    gchar *quoted_full_path = g_shell_quote(op->file_path);
                    gchar *command_line = g_strdup_printf("exiftool -j -Title -Year -Category %s", quoted_full_path);
                    SAFE_FREE(quoted_full_path);

                    gchar **argv = NULL;
                    gint argc = 0;
                    GError *spawn_error = NULL;
                    gboolean success = g_shell_parse_argv(command_line, &argc, &argv, &spawn_error);
                    if (!success) {
                        g_warning("Failed to parse exiftool command line: %s (%s)", command_line, spawn_error ? spawn_error->message : "N/A");
                        if (spawn_error) g_error_free(spawn_error);
                        SAFE_FREE(command_line);
                        SAFE_FREE(op->file_path);
                        SAFE_FREE(op);
                        SAFE_FREE(lower_case_filename);
                        if (file_info) g_object_unref(file_info);
                        if (file_info_error) g_error_free(file_info_error);
                        if (file) g_object_unref(file);
                        SAFE_FREE(full_path);
                        continue;
                    }

                    GPid pid;
                    gint stdout_fd_local, stderr_fd_local;
                    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                                  NULL, NULL, &pid, NULL, &stdout_fd_local, &stderr_fd_local, &spawn_error)) {
                        g_warning("Failed to spawn exiftool process: %s", spawn_error ? spawn_error->message : "N/A");
                        if (spawn_error) g_error_free(spawn_error);
                        SAFE_FREE(op->file_path);
                        SAFE_FREE(op);
                        g_strfreev(argv);
                        SAFE_FREE(command_line);
                        SAFE_FREE(lower_case_filename);
                        if (file_info) g_object_unref(file_info);
                        if (file_info_error) g_error_free(file_info_error);
                        if (file) g_object_unref(file);
                        SAFE_FREE(full_path);
                        continue;
                    }

                    op->stdout_fd = stdout_fd_local;
                    op->stderr_fd = stderr_fd_local;

                    g_strfreev(argv);
                    SAFE_FREE(command_line);

                    g_child_watch_add(pid, exiftool_read_completed_callback, op);
                }
                SAFE_FREE(lower_case_filename);
            }
            if (file_info) g_object_unref(file_info);
            if (file_info_error) g_error_free(file_info_error);
            if (file) g_object_unref(file);
            SAFE_FREE(full_path);
        }
        closedir(d);
    } else {
        g_warning("Could not open category directory: %s (%s). Skipping this category.", folder_path, g_strerror(errno));
    }
}

/**
 * @brief Saves a single PDF entry, copies file, and injects metadata.
 */
gboolean save_single_pdf_entry(PdfEntry *entry, const gchar *professor_name, gint category_index) {
    const gchar *title_raw = gtk_entry_get_text(GTK_ENTRY(entry->title_entry));
    const gchar *year = gtk_entry_get_text(GTK_ENTRY(entry->year_entry));
    const gchar *manual_counter_str = gtk_entry_get_text(GTK_ENTRY(entry->counter_entry));
    const gchar *current_category_text = gtk_entry_get_text(GTK_ENTRY(entry->category_entry));

    if (!entry->file_path || g_strcmp0(entry->file_path, "") == 0 || !is_valid_path(entry->file_path)) {
        g_warning("Skipping entry in category '%s': No valid file selected.", global_categories[category_index]);
        return FALSE;
    }
    if (g_strcmp0(title_raw, "") == 0) {
         g_warning("Skipping entry in category '%s' (file: %s): Title is empty.", global_categories[category_index], entry->file_path);
         return FALSE;
    }
    if (g_strcmp0(year, "") == 0) {
         g_warning("Skipping entry in category '%s' (file: %s): Year is empty.", global_categories[category_index], entry->file_path);
         return FALSE;
    }
    if (g_strcmp0(current_category_text, "") == 0) {
         g_warning("Skipping entry in category '%s' (file: %s): Category is empty.", global_categories[category_index], entry->file_path);
         return FALSE;
    }

    g_print("    Original File Selected: %s\n", entry->file_path);
    g_print("    Entered Title: \"%s\", Entered Year: \"%s\", Entered Category: \"%s\", Manual Counter: \"%s\"\n",
            title_raw, year, current_category_text, manual_counter_str);

    gchar *sanitized_category_for_filename = sanitize_filename(current_category_text);

    gchar *final_filename_no_ext = NULL;
    gint current_counter = 0;
    gboolean use_manual_counter_for_filename = FALSE;

    if (manual_counter_str && g_strcmp0(manual_counter_str, "") != 0) {
        gint parsed_counter = atoi(manual_counter_str);
        if (parsed_counter > 0) {
            current_counter = parsed_counter;
            use_manual_counter_for_filename = TRUE;
            g_print("    Using manual counter: %02d\n", current_counter);
        }
    }

    if (use_manual_counter_for_filename) {
        final_filename_no_ext = g_strdup_printf("%02d_%s_%s", current_counter, sanitized_category_for_filename, year);
    } else {
        if (entry->has_original_counter && entry->original_detected_counter > 0) {
            current_counter = entry->original_detected_counter;
            g_print("    Using detected original counter: %02d\n", current_counter);
        } else {
            global_max_category_counters[category_index]++;
            current_counter = global_max_category_counters[category_index];
            g_print("    Assigning new counter: %02d\n", current_counter);
        }
        final_filename_no_ext = g_strdup_printf("%02d_%s_%s", current_counter, sanitized_category_for_filename, year);
    }

    gchar *professor_base_dir = g_strdup("./");
    gchar *dest_category_dir = g_strdup_printf("%s%s/%s", professor_base_dir, professor_name, global_categories[category_index]);
    g_print("    Attempting to ensure category directory exists: %s\n", dest_category_dir);

    if (!is_valid_path(dest_category_dir)) {
        g_warning("Invalid destination category directory path: %s", dest_category_dir);
        SAFE_FREE(sanitized_category_for_filename);
        SAFE_FREE(final_filename_no_ext);
        SAFE_FREE(dest_category_dir);
        SAFE_FREE(professor_base_dir);
        return FALSE;
    }

    if (!g_mkdir_with_parents(dest_category_dir, 0755)) {
        if (errno != EEXIST) {
            g_warning("Failed to create category directory: %s (%s)", dest_category_dir, g_strerror(errno));
            SAFE_FREE(sanitized_category_for_filename);
            SAFE_FREE(final_filename_no_ext);
            SAFE_FREE(dest_category_dir);
            SAFE_FREE(professor_base_dir);
            return FALSE;
        }
    }
    g_print("    Category directory exists (or was created): %s\n", dest_category_dir);

    gchar *dest_file_name = g_strdup_printf("%s.pdf", final_filename_no_ext);
    gchar *dest_file_path = g_strdup_printf("%s/%s", dest_category_dir, dest_file_name);
    g_print("    Destination file path: %s\n", dest_file_path);

    if (!is_valid_path(dest_file_path)) {
        g_warning("Invalid destination file path: %s", dest_file_path);
        SAFE_FREE(sanitized_category_for_filename);
        SAFE_FREE(final_filename_no_ext);
        SAFE_FREE(dest_file_name);
        SAFE_FREE(dest_file_path);
        SAFE_FREE(dest_category_dir);
        SAFE_FREE(professor_base_dir);
        return FALSE;
    }

    GFile *source_file = g_file_new_for_path(entry->file_path);
    GFile *destination_file = g_file_new_for_path(dest_file_path);

    GError *copy_error = NULL;
    if (!g_file_copy(source_file, destination_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &copy_error)) {
        g_warning("Failed to copy file from %s to %s: %s", entry->file_path, dest_file_path, copy_error ? copy_error->message : "N/A");
        if (copy_error) g_error_free(copy_error);
        if (source_file) g_object_unref(source_file);
        if (destination_file) g_object_unref(destination_file);
        SAFE_FREE(sanitized_category_for_filename);
        SAFE_FREE(final_filename_no_ext);
        SAFE_FREE(dest_file_name);
        SAFE_FREE(dest_file_path);
        SAFE_FREE(dest_category_dir);
        SAFE_FREE(professor_base_dir);
        return FALSE;
    }
    g_print("    File successfully copied to: %s\n", dest_file_path);

    if (source_file) g_object_unref(source_file);
    if (destination_file) g_object_unref(destination_file);

    gchar *exiftool_command_line = g_strdup_printf("exiftool -charset utf8 -Title=\"%s\" -Year=\"%s\" -Category=\"%s\" -overwrite_original_in_place \"%s\"",
                                              title_raw, year, current_category_text, dest_file_path);
    g_print("    Executing exiftool command to write metadata: %s\n", exiftool_command_line);

    gchar **exiftool_argv = NULL;
    gint exiftool_argc = 0;
    GError *exiftool_error = NULL;
    gboolean exiftool_success = g_shell_parse_argv(exiftool_command_line, &exiftool_argc, &exiftool_argv, &exiftool_error);
    if (!exiftool_success) {
        g_warning("Failed to parse exiftool write command line: %s (%s)", exiftool_command_line, exiftool_error ? exiftool_error->message : "N/A");
        if (exiftool_error) g_error_free(exiftool_error);
        SAFE_FREE(exiftool_command_line);
        SAFE_FREE(sanitized_category_for_filename);
        SAFE_FREE(final_filename_no_ext);
        SAFE_FREE(dest_file_name);
        SAFE_FREE(dest_file_path);
        SAFE_FREE(dest_category_dir);
        SAFE_FREE(professor_base_dir);
        return FALSE;
    }

    gchar *exiftool_stdout = NULL;
    gchar *exiftool_stderr = NULL;
    gint exiftool_exit_status = 0;

    exiftool_success = g_spawn_sync(
        NULL,
        exiftool_argv,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        &exiftool_stdout,
        &exiftool_stderr,
        &exiftool_exit_status,
        &exiftool_error
    );

    if (exiftool_argv) g_strfreev(exiftool_argv);
    SAFE_FREE(exiftool_command_line);
    SAFE_FREE(exiftool_stdout);
    SAFE_FREE(exiftool_stderr);

    if (!exiftool_success || exiftool_exit_status != 0) {
        g_warning("Failed to update metadata for %s. Exiftool command failed with exit code: %d. Error: %s",
                  dest_file_path, exiftool_exit_status, exiftool_error ? exiftool_error->message : "N/A");
        if (exiftool_error) g_error_free(exiftool_error);
    } else {
        g_print("    Metadata successfully written to %s.\n", dest_file_path);
    }

    gboolean filename_changed = (entry->original_filename_no_ext == NULL || g_strcmp0(final_filename_no_ext, entry->original_filename_no_ext) != 0);

    if (filename_changed && entry->file_path != NULL && g_file_test(entry->file_path, G_FILE_TEST_EXISTS)) {
        gchar *old_files_dir = g_strdup_printf("%s/old_files", dest_category_dir);
        g_print("    Attempting to ensure old_files directory exists: %s\n", old_files_dir);

        if (!is_valid_path(old_files_dir)) {
            g_warning("Invalid old_files directory path: %s", old_files_dir);
            SAFE_FREE(old_files_dir);
        } else {
            if (!g_mkdir_with_parents(old_files_dir, 0755)) {
                if (errno != EEXIST) {
                    g_warning("Failed to create old_files directory: %s (%s)", old_files_dir, g_strerror(errno));
                }
            } else {
                g_print("    Old_files directory exists (or was created): %s\n", old_files_dir);
            }

            gchar *original_basename = g_path_get_basename(entry->file_path);
            gchar *old_file_dest_path = g_strdup_printf("%s/%s", old_files_dir, original_basename);
            g_print("    Moving original file from %s to %s\n", entry->file_path, old_file_dest_path);

            if (!is_valid_path(old_file_dest_path)) {
                g_warning("Invalid old file destination path: %s", old_file_dest_path);
                SAFE_FREE(original_basename);
                SAFE_FREE(old_file_dest_path);
                SAFE_FREE(old_files_dir);
            } else {
                GError *move_error = NULL;
                GFile *original_gfile_to_move = g_file_new_for_path(entry->file_path);
                GFile *old_dest_gfile = g_file_new_for_path(old_file_dest_path);

                if (g_file_query_exists(original_gfile_to_move, NULL)) {
                    if (!g_file_move(original_gfile_to_move, old_dest_gfile, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &move_error)) {
                        g_warning("Failed to move original file from %s to %s: %s", entry->file_path, old_file_dest_path, move_error ? move_error->message : "N/A");
                        if (move_error) g_error_free(move_error);
                    } else {
                        g_print("    Original file successfully moved to: %s\n", old_file_dest_path);
                    }
                } else {
                    g_warning("Original file %s does not exist (perhaps already moved or deleted), skipping move to old_files.", entry->file_path);
                }

                if (original_gfile_to_move) g_object_unref(original_gfile_to_move);
                if (old_dest_gfile) g_object_unref(old_dest_gfile);
                SAFE_FREE(original_basename);
                SAFE_FREE(old_file_dest_path);
                SAFE_FREE(old_files_dir);
            }
        }
    } else {
        g_print("    Filename unchanged or original file not found, skipping move to old_files for %s.\n", entry->file_path ? entry->file_path : "N/A");
    }

    SAFE_FREE(sanitized_category_for_filename);
    SAFE_FREE(final_filename_no_ext);
    SAFE_FREE(dest_file_name);
    SAFE_FREE(dest_file_path);
    SAFE_FREE(dest_category_dir);
    SAFE_FREE(professor_base_dir);

    return TRUE;
}

/**
 * @brief Callback when report generation finishes. Displays status, opens PDF.
 */
void on_report_generation_finished(GPid pid, gint status, gpointer user_data) {
    gchar *professor_name = (gchar *)user_data;
    g_print("Report generation process (PID: %d) finished with status: %d\n", pid, status);

    g_spawn_close_pid(pid);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_INFO,
                                                   GTK_BUTTONS_OK,
                                                   "Relatório gerado com sucesso!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        gchar *sanitized_professor_name = sanitize_filename(professor_name);
        gchar *pdf_filename = g_strdup_printf("final/%s_relatorio_profissional.pdf", sanitized_professor_name);
        GError *spawn_error = NULL;

        if (g_file_test(pdf_filename, G_FILE_TEST_EXISTS)) {
            gchar *xdg_open_command = g_strdup_printf("xdg-open \"%s\"", pdf_filename);
            g_print("Attempting to open PDF with command: %s\n", xdg_open_command);

            if (!g_spawn_command_line_async(xdg_open_command, &spawn_error)) {
                g_warning("Failed to launch PDF viewer via xdg-open: %s", spawn_error ? spawn_error->message : "N/A");
                if (spawn_error) g_error_free(spawn_error);
            } else {
                g_print("PDF viewer launched successfully (via xdg-open).\n");
            }
            SAFE_FREE(xdg_open_command);
            if (preview_report_btn) {
                gtk_widget_show(preview_report_btn);
            }
        } else {
            g_warning("Generated PDF not found at path: %s", pdf_filename);
            if (preview_report_btn) {
                gtk_widget_hide(preview_report_btn);
            }
        }
        SAFE_FREE(pdf_filename);
        SAFE_FREE(sanitized_professor_name);

        gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 1.0);
        gchar *progress_text_100 = g_strdup_printf("%d%%", 100);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_100);
        SAFE_FREE(progress_text_100);

    } else {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Falha ao gerar o relatório. Verifique o log do script no terminal que foi aberto.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_warning("Backend script failed with exit status: %d", status);
        gtk_label_set_text(GTK_LABEL(status_label), "Erro na geração do relatório.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error);
        if (preview_report_btn) {
            gtk_widget_hide(preview_report_btn);
        }
    }
    SAFE_FREE(professor_name);
}

/**
 * @brief Orchestrates report generation: saves data, spawns backend script.
 */
void generate_report(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));
    if(!professor) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Por favor, selecione um professor.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    g_print("Generating report for professor: %s\n", professor);

    if (preview_report_btn) {
        gtk_widget_hide(preview_report_btn);
    }

    gtk_label_set_text(GTK_LABEL(status_label), "Iniciando geração do relatório...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.2);
    gchar *progress_text_20 = g_strdup_printf("%d%%", 20);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_20);
    SAFE_FREE(progress_text_20);
    g_main_context_iteration(NULL, FALSE);

    gchar *professor_base_dir = g_strdup("./");
    g_print("  Base directory for professor folders: %s\n", professor_base_dir);

    gchar *professor_folder_path = g_strdup_printf("%s%s", professor_base_dir, professor);
    g_print("  Attempting to ensure professor directory exists: %s\n", professor_folder_path);

    if (!is_valid_path(professor_folder_path)) {
        g_warning("Invalid professor folder path: %s", professor_folder_path);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Caminho inválido para o diretório do professor: %s", professor_folder_path);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        SAFE_FREE(professor_folder_path);
        SAFE_FREE(professor_base_dir);
        gtk_label_set_text(GTK_LABEL(status_label), "Erro ao criar diretório do professor.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error);
        g_main_context_iteration(NULL, FALSE);
        return;
    }

    if (!g_mkdir_with_parents(professor_folder_path, 0755)) {
        if (errno != EEXIST) {
            GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                       GTK_DIALOG_MODAL,
                                                       GTK_MESSAGE_ERROR,
                                                       GTK_BUTTONS_OK,
                                                       "Falha ao criar o diretório do professor: %s (%s)",
                                                       professor_folder_path, g_strerror(errno));
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            SAFE_FREE(professor_folder_path);
            SAFE_FREE(professor_base_dir);
            gtk_label_set_text(GTK_LABEL(status_label), "Erro ao criar diretório do professor.");
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
            gchar *progress_text_error = g_strdup_printf("Erro!");
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
            SAFE_FREE(progress_text_error);
            g_main_context_iteration(NULL, FALSE);
            return;
        }
    }
    g_print("  Professor directory exists (or was created): %s\n", professor_folder_path);
    SAFE_FREE(professor_folder_path);

    gint total_pdfs_to_process = 0;
    for (gint i = 0; i < global_num_categories; i++) {
        total_pdfs_to_process += g_list_length(global_sections[i].entries);
    }
    g_print("Total PDFs to process: %d\n", total_pdfs_to_process);

    gint processed_pdfs_count = 0;

    for (gint i = 0; i < global_num_categories; i++) {
        g_print("Processing category: %s\n", global_categories[i]);
        GList *l;
        for (l = global_sections[i].entries; l != NULL; l = g_list_next(l)) {
            PdfEntry *entry = (PdfEntry *)l->data;
            if (save_single_pdf_entry(entry, professor, i)) {
                processed_pdfs_count++;
                gdouble fraction_of_processing = (gdouble)processed_pdfs_count / total_pdfs_to_process;
                gdouble overall_fraction = 0.2 + (fraction_of_processing * 0.7);
                gchar *progress_text = g_strdup_printf("Processando PDF %d de %d: %s",
                                                       processed_pdfs_count, total_pdfs_to_process, gtk_entry_get_text(GTK_ENTRY(entry->title_entry)));
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), overall_fraction);
                gchar *progress_percent_text = g_strdup_printf("%d%%", (gint)(overall_fraction * 100));
                gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_percent_text);
                SAFE_FREE(progress_percent_text);
                gtk_label_set_text(GTK_LABEL(status_label), progress_text);
                SAFE_FREE(progress_text);
                g_main_context_iteration(NULL, FALSE);
            } else {
                g_warning("Failed to save PDF entry: %s", entry->file_path ? entry->file_path : "N/A");
            }
        }
    }

    SAFE_FREE(professor_base_dir);

    gchar *terminal_path = g_find_program_in_path("gnome-terminal");
    if (!terminal_path) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Erro: O programa 'gnome-terminal' não foi encontrado no seu PATH.\n"
                                                   "Por favor, certifique-se de que está instalado e acessível.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        gtk_label_set_text(GTK_LABEL(status_label), "Erro: Terminal não encontrado.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error);
        if (preview_report_btn) {
            gtk_widget_hide(preview_report_btn);
        }
        return;
    }

    gchar *script_path = g_strdup("./generate_pdf_professor_v2.0.1-R22.sh");
    gchar *professor_arg_quoted = g_shell_quote(professor);

    gchar *command_in_terminal = g_strdup_printf("%s %s; echo \"\"; echo \"Pressione Enter para fechar esta janela...\"; read -n 1",
                                                  script_path, professor_arg_quoted);

    gchar *argv_terminal[] = {
        terminal_path,
        g_strdup("--wait"),
        g_strdup("--"),
        g_strdup("bash"),
        g_strdup("-c"),
        command_in_terminal,
        NULL
    };

    GPid child_pid;
    GError *spawn_error = NULL;

    gtk_label_set_text(GTK_LABEL(status_label), "Abrindo terminal para compilar relatório LaTeX...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.9);
    gchar *progress_text_90 = g_strdup_printf("%d%%", 90);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_90);
    SAFE_FREE(progress_text_90);
    g_main_context_iteration(NULL, FALSE);

    if (!g_spawn_async(NULL,
                       argv_terminal,
                       NULL,
                       G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL,
                       NULL,
                       &child_pid,
                       &spawn_error)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Falha ao iniciar o terminal para geração de relatório: %s", spawn_error ? spawn_error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (spawn_error) g_error_free(spawn_error);
        gtk_label_set_text(GTK_LABEL(status_label), "Erro ao iniciar geração.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error_again = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error_again);
        SAFE_FREE(progress_text_error_again);
        if (preview_report_btn) {
            gtk_widget_hide(preview_report_btn);
        }
    } else {
        g_print("Terminal spawned successfully with PID: %d\n", child_pid);
        g_child_watch_add(child_pid, on_report_generation_finished, g_strdup(professor));
    }

    SAFE_FREE(terminal_path);
    SAFE_FREE(argv_terminal[1]);
    SAFE_FREE(argv_terminal[2]);
    SAFE_FREE(argv_terminal[3]);
    SAFE_FREE(argv_terminal[4]);
    SAFE_FREE(argv_terminal[5]);
    SAFE_FREE(script_path);
    SAFE_FREE(professor_arg_quoted);
}

/**
 * @brief Saves all current PDF entries by updating files and metadata.
 */
void on_save_all_entries(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));
    if (!professor) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Por favor, selecione um professor para salvar as alterações.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    g_print("Saving all entries for professor: %s\n", professor);

    gtk_label_set_text(GTK_LABEL(status_label), "Salvando alterações...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gchar *progress_text_0 = g_strdup_printf("%d%%", 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0);
    SAFE_FREE(progress_text_0);
    g_main_context_iteration(NULL, FALSE);

    gint total_pdfs_to_save = 0;
    for (gint i = 0; i < global_num_categories; i++) {
        total_pdfs_to_save += g_list_length(global_sections[i].entries);
    }

    if (total_pdfs_to_save == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_INFO,
                                                   GTK_BUTTONS_OK,
                                                   "Não há PDFs para salvar.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_0_again = g_strdup_printf("%d%%", 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0_again);
        SAFE_FREE(progress_text_0_again);
        g_main_context_iteration(NULL, FALSE);
        return;
    }

    gchar *exiftool_path = g_find_program_in_path("exiftool");
    if (!exiftool_path) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Erro: O programa 'exiftool' não foi encontrado no seu PATH.\n"
                                                   "Não foi possível salvar os metadados dos PDFs. Por favor, instale-o (sudo apt install libimage-exiftool-perl) e tente novamente.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        gtk_label_set_text(GTK_LABEL(status_label), "Erro: exiftool ausente.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error);
        g_main_context_iteration(NULL, FALSE);
        SAFE_FREE(exiftool_path);
        return;
    }
    SAFE_FREE(exiftool_path);

    gint saved_pdfs_count = 0;
    for (gint i = 0; i < global_num_categories; i++) {
        GList *l;
        for (l = global_sections[i].entries; l != NULL; l = g_list_next(l)) {
            PdfEntry *entry = (PdfEntry *)l->data;
            if (save_single_pdf_entry(entry, professor, i)) {
                saved_pdfs_count++;
            } else {
                g_warning("Failed to save PDF entry during 'Salvar Alterações': %s", entry->file_path ? entry->file_path : "N/A");
            }

            gdouble fraction = (gdouble)saved_pdfs_count / total_pdfs_to_save;
            gchar *progress_text = g_strdup_printf("Salvando PDF %d de %d: %s",
                                                   saved_pdfs_count, total_pdfs_to_save, gtk_entry_get_text(GTK_ENTRY(entry->title_entry)));
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), fraction);
            gchar *progress_percent_text = g_strdup_printf("%d%%", (gint)(fraction * 100));
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_percent_text);
            SAFE_FREE(progress_percent_text);
            gtk_label_set_text(GTK_LABEL(status_label), progress_text);
            SAFE_FREE(progress_text);
            g_main_context_iteration(NULL, FALSE);
        }
    }

    GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK,
                                               "Alterações salvas com sucesso!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    gchar *sanitized_professor_name = sanitize_filename(professor);
    gchar *pdf_filename = g_strdup_printf("final/%s_relatorio_profissional.pdf", sanitized_professor_name);
    if (g_file_test(pdf_filename, G_FILE_TEST_EXISTS)) {
        if (preview_report_btn) {
            gtk_widget_show(preview_report_btn);
        }
    } else {
        if (preview_report_btn) {
            gtk_widget_hide(preview_report_btn);
        }
    }
    SAFE_FREE(pdf_filename);
    SAFE_FREE(sanitized_professor_name);

    gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 1.0);
    gchar *progress_text_100 = g_strdup_printf("%d%%", 100);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_100);
    SAFE_FREE(progress_text_100);
    g_main_context_iteration(NULL, FALSE);

    g_print("Scheduling auto-reload of professor after saving changes...\n");
    g_idle_add(on_professor_selected_idle_wrapper, professor_combo);
}

/**
 * @brief Wrapper for on_professor_selected to use with g_idle_add.
 */
gboolean on_professor_selected_idle_wrapper(gpointer user_data) {
    GtkComboBox *combo_box = GTK_COMBO_BOX(user_data);
    return on_professor_selected(combo_box, NULL);
}

/**
 * @brief Creates professor's main and category subdirectories.
 */
void on_create_professor_folder_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));
    if (!professor || g_strcmp0(professor, "") == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Por favor, selecione um professor para criar a pasta.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    g_print("Attempting to create folders for professor: %s\n", professor);

    gchar *professor_base_dir = g_strdup("./");
    gchar *professor_folder_path = g_strdup_printf("%s%s", professor_base_dir, professor);
    gboolean success = TRUE;
    gchar *message = NULL;

    if (!g_mkdir_with_parents(professor_folder_path, 0755)) {
        if (errno != EEXIST) {
            message = g_strdup_printf("Falha ao criar o diretório do professor '%s': %s",
                                      professor_folder_path, g_strerror(errno));
            success = FALSE;
        } else {
            g_print("  Diretório do professor '%s' já existe.\n", professor_folder_path);
        }
    } else {
        g_print("  Diretório do professor '%s' criado.\n", professor_folder_path);
    }

    if (success) {
        for (gint i = 0; i < global_num_categories; i++) {
            gchar *category_folder_path = g_strdup_printf("%s/%s", professor_folder_path, global_categories[i]);
            if (!g_mkdir_with_parents(category_folder_path, 0755)) {
                if (errno != EEXIST) {
                    message = g_strdup_printf("Falha ao criar o diretório da categoria '%s' para '%s': %s",
                                              global_categories[i], professor, g_strerror(errno));
                    success = FALSE;
                    SAFE_FREE(category_folder_path);
                    break;
                } else {
                    g_print("  Diretório da categoria '%s' já existe para '%s'.\n",
                              global_categories[i], professor);
                }
            } else {
                g_print("  Diretório da categoria '%s' criado para '%s'.\n",
                          global_categories[i], professor);
            }
            SAFE_FREE(category_folder_path);
        }
    }

    GtkWidget *dialog;
    if (success) {
        dialog = gtk_message_dialog_new(NULL,
                                        GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_INFO,
                                        GTK_BUTTONS_OK,
                                        "Pastas criadas com sucesso para o professor '%s'!", professor);
    } else {
        dialog = gtk_message_dialog_new(NULL,
                                        GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_ERROR,
                                        GTK_BUTTONS_OK,
                                        "Erro ao criar pastas: %s", message ? message : "Erro desconhecido.");
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    SAFE_FREE(professor_folder_path);
    SAFE_FREE(professor_base_dir);
    SAFE_FREE(message);

    on_professor_selected(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Opens the PDF associated with a PdfEntry.
 */
void on_preview_pdf(GtkWidget *button, PdfEntry *entry) {
    (void)button;

    if (!entry || !entry->file_path || g_strcmp0(entry->file_path, "") == 0) {
        g_warning("No file path available for preview.");
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_WARNING,
                                                   GTK_BUTTONS_OK,
                                                   "Nenhum arquivo PDF selecionado para visualizar.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (!is_valid_path(entry->file_path)) {
        g_warning("Invalid file path for preview: %s", entry->file_path);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Caminho do arquivo inválido: %s", entry->file_path);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    if (!g_file_test(entry->file_path, G_FILE_TEST_EXISTS)) {
        g_warning("File does not exist for preview: %s", entry->file_path);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "O arquivo não existe: %s", entry->file_path);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    g_print("Attempting to open PDF for preview: %s\n", entry->file_path);
    GError *error = NULL;
    gchar *command_line = g_strdup_printf("xdg-open \"%s\"", entry->file_path);

    if (!g_spawn_command_line_async(command_line, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Falha ao abrir o PDF: %s", error ? error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error);
    } else {
        g_print("PDF opened successfully for preview.\n");
    }
    SAFE_FREE(command_line);
}

/**
 * @brief Opens the generated professional report PDF.
 */
void on_preview_report_pdf(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));
    if (!professor) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Por favor, selecione um professor para visualizar o relatório.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    gchar *sanitized_professor_name = sanitize_filename(professor);
    gchar *pdf_filename = g_strdup_printf("final/%s_relatorio_profissional.pdf", sanitized_professor_name);

    if (!is_valid_path(pdf_filename)) {
        g_warning("Invalid PDF report file path: %s", pdf_filename);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Caminho do relatório inválido: %s", pdf_filename);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        SAFE_FREE(pdf_filename);
        SAFE_FREE(sanitized_professor_name);
        return;
    }

    if (!g_file_test(pdf_filename, G_FILE_TEST_EXISTS)) {
        g_warning("Professional report PDF does not exist: %s", pdf_filename);
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_WARNING,
                                                   GTK_BUTTONS_OK,
                                                   "O relatório profissional para '%s' ainda não foi gerado ou não foi encontrado em '%s'.\n"
                                                   "Por favor, gere o relatório primeiro.",
                                                   professor, pdf_filename);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        SAFE_FREE(pdf_filename);
        SAFE_FREE(sanitized_professor_name);
        return;
    }

    g_print("Attempting to open professional report PDF: %s\n", pdf_filename);
    GError *error = NULL;
    gchar *command_line = g_strdup_printf("xdg-open \"%s\"", pdf_filename);

    if (!g_spawn_command_line_async(command_line, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Falha ao abrir o relatório: %s", error ? error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error);
    } else {
        g_print("Professional report PDF opened successfully.\n");
    }

    SAFE_FREE(command_line);
    SAFE_FREE(pdf_filename);
    SAFE_FREE(sanitized_professor_name);
}

/**
 * @brief Reloads PDF entries for the current professor.
 */
void on_reload_professor_button_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    g_print("Reload professor button clicked. Triggering on_professor_selected.\n");
    on_professor_selected(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Scrolls to the corresponding category frame.
 */
void on_category_shortcut_clicked(GtkWidget *button, gpointer user_data) {
    (void)user_data;

    GtkWidget *target_frame = g_object_get_data(G_OBJECT(button), "category-frame");

    if (target_frame) {
        g_print("Category shortcut clicked. Scrolling to frame: %s\n", gtk_frame_get_label(GTK_FRAME(target_frame)));
        GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(main_category_scrolled_window));

        GtkAllocation allocation;
        gtk_widget_get_allocation(target_frame, &allocation);

        gdouble target_y = (gdouble)allocation.y;

        gtk_adjustment_set_value(vadjustment, target_y);
    } else {
        g_warning("Target category frame not found for shortcut button.");
    }
}

/**
 * @brief Displays the configuration dialog.
 */
void show_config_dialog(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Configurações",
                                                    NULL,
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Fechar", GTK_RESPONSE_CLOSE,
                                                    NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 500);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(content_area), notebook, TRUE, TRUE, 0);

    GError *error = NULL;
    gchar *file_content = NULL;

    GtkWidget *professors_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(professors_vbox), 10);
    gtk_widget_set_hexpand(professors_vbox, TRUE);
    gtk_widget_set_vexpand(professors_vbox, TRUE);

    GtkWidget *professors_label = gtk_label_new("Lista de Professores (um por linha):");
    gtk_box_pack_start(GTK_BOX(professors_vbox), professors_label, FALSE, FALSE, 0);

    professors_text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(professors_text_view), GTK_WRAP_WORD);
    GtkWidget *prof_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(prof_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(prof_scrolled_window, TRUE);
    gtk_widget_set_vexpand(prof_scrolled_window, TRUE);
    gtk_container_add(GTK_CONTAINER(prof_scrolled_window), professors_text_view);
    gtk_box_pack_start(GTK_BOX(professors_vbox), prof_scrolled_window, TRUE, TRUE, 5);

    file_content = NULL;
    error = NULL;
    if (g_file_get_contents("config/professores.txt", &file_content, NULL, &error)) {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(professors_text_view)), file_content, -1);
        SAFE_FREE(file_content);
    } else {
        g_warning("Could not read 'config/professores.txt' for config dialog: %s", error ? error->message : "N/A");
        if (error) g_error_free(error);
    }

    GtkWidget *save_professors_btn = gtk_button_new_with_label("Salvar Professores");
    g_signal_connect(save_professors_btn, "clicked", G_CALLBACK(save_professors_list), NULL);
    gtk_box_pack_start(GTK_BOX(professors_vbox), save_professors_btn, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), professors_vbox, gtk_label_new("Professores"));

    GtkWidget *categories_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(categories_vbox), 10);
    gtk_widget_set_hexpand(categories_vbox, TRUE);
    gtk_widget_set_vexpand(categories_vbox, TRUE);

    GtkWidget *categories_label = gtk_label_new("Lista de Categorias (um por linha):");
    gtk_box_pack_start(GTK_BOX(categories_vbox), categories_label, FALSE, FALSE, 0);

    categories_text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(categories_text_view), GTK_WRAP_WORD);
    GtkWidget *cat_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cat_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(cat_scrolled_window, TRUE);
    gtk_widget_set_vexpand(cat_scrolled_window, TRUE);
    gtk_container_add(GTK_CONTAINER(cat_scrolled_window), categories_text_view);
    gtk_box_pack_start(GTK_BOX(categories_vbox), cat_scrolled_window, TRUE, TRUE, 5);

    file_content = NULL;
    error = NULL;
    if (g_file_get_contents("config/categories.txt", &file_content, NULL, &error)) {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(categories_text_view)), file_content, -1);
        SAFE_FREE(file_content);
    } else {
        g_warning("Could not read 'config/categories.txt' for config dialog: %s", error ? error->message : "N/A");
        if (error) g_error_free(error);
    }

    GtkWidget *save_categories_btn = gtk_button_new_with_label("Salvar Categorias");
    g_signal_connect(save_categories_btn, "clicked", G_CALLBACK(save_categories_list), NULL);
    gtk_box_pack_start(GTK_BOX(categories_vbox), save_categories_btn, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), categories_vbox, gtk_label_new("Categorias"));

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/**
 * @brief Callback for closing the help dialog.
 */
void on_help_dialog_close_clicked(GtkWidget *button, GtkWidget *dialog) {
    (void)button;
    gtk_widget_destroy(dialog);
}

/**
 * @brief Displays a help dialog with application instructions.
 */
void on_help_button_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Ajuda do Gerador de Relatórios",
                                                    NULL,
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "Fechar", GTK_RESPONSE_CLOSE,
                                                    NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 600);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_box_pack_start(GTK_BOX(content_area), vbox, TRUE, TRUE, 0);

    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_set_text(buffer,
                             "Ajuda do Gerador de Relatórios de Professor\n\n"
                             "Este aplicativo ajuda a organizar e gerar relatórios profissionais em PDF para professores.\n\n"
                             "Funcionalidades:\n"
                             "1.  **Seleção de Professor**: Escolha um professor na lista suspensa. Isso carregará os PDFs existentes associados a ele.\n"
                             "2.  **Seções de Categoria**: Os PDFs são organizados por categorias. Cada seção de categoria contém uma lista de PDFs.\n"
                             "3.  **Adicionar PDF**: Clique em 'Adicionar PDF' em uma seção de categoria para adicionar um novo documento. Selecione o arquivo PDF e o aplicativo tentará extrair metadados.\n"
                             "4.  **Editar Metadados**: Você pode editar o Título, Ano, Contador e Categoria de cada entrada de PDF diretamente nos campos de texto.\n"
                             "    * O **Contador** determina a ordem na qual os documentos aparecerão no relatório.\n"
                             "    * A **Categoria** é usada para agrupar documentos e pode ser alterada para mover um documento entre categorias.\n"
                             "5.  **Visualizar PDF**: Clique em 'Visualizar' para abrir o PDF com o visualizador padrão do sistema.\n"
                             "6.  **Remover PDF**: Clique em 'Remover' para excluir uma entrada da lista e mover o arquivo PDF original para uma pasta 'old_files' dentro da categoria do professor.\n"
                             "7.  **Salvar Alterações**: O botão 'Salvar Alterações' salva os metadados e os arquivos PDF nas pastas corretas do professor (dentro do diretório atual da aplicação). Ele renomeia os arquivos para 'NN_Categoria_AAAA.pdf'.\n"
                             "8.  **Gerar Relatório Profissional**: Após salvar as alterações, clique em 'Gerar Relatório Profissional'. Isso executará um script LaTeX de backend para compilar um relatório consolidado.\n"
                             "    * Um novo terminal será aberto para mostrar o progresso da compilação. NÃO FECHE este terminal até que a compilação seja concluída.\n"
                             "    * O relatório final será salvo em 'final/<Nome_Professor>_relatorio_profissional.pdf'.\n"
                             "9.  **Visualizar Relatório Profissional**: Após a geração, este botão aparecerá para abrir o relatório final.\n"
                             "10. **Configurações**: O botão 'Configurações' permite adicionar/remover professores e categorias editando os arquivos de configuração (config/professores.txt e config/categories.txt).\n"
                             "11. **Recarregar Professor**: Recarrega a lista de PDFs para o professor atualmente selecionado, útil após alterações manuais nas pastas de arquivos.\n"
                             "12. **Criar Pasta do Professor**: Cria a estrutura de pastas para o professor selecionado (pasta principal e subpastas de categoria) se elas não existirem.\n\n"
                             "Estrutura de Pastas Esperada:\n"
                             "  ./\n"
                             "  ├── config/\n"
                             "  │   ├── categories.txt\n"
                             "  │   └── professores.txt\n"
                             "  ├── final/\n"
                             "  │   └── <Nome_Professor>_relatorio_profissional.pdf\n"
                             "  ├── <Nome_Professor_1>/\n"
                             "  │   ├── <Categoria_1>/\n"
                             "  │   │   ├── 01_Categoria_Ano.pdf\n"
                             "  │   │   └── old_files/\n"
                             "  │   └── <Categoria_2>/\n"
                             "  │       └── ...\n"
                             "  └── <Nome_Professor_2>/\n"
                             "      └── ...\n\n"
                             "Dependências:\n"
                             "  * GTK+ 3.x\n"
                             "  * exiftool (sudo apt install libimage-exiftool-perl)\n"
                             "  * xdg-utils (para xdg-open)\n"
                             "  * LaTeX distribution (e.g., TeX Live) e pdflatex (para compilação do relatório)\n"
                             "  * gnome-terminal (ou similar, para executar o script de backend em uma janela separada)\n",
                             -1);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_window), text_view);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    // Removed the deprecated gtk_dialog_get_action_area usage.
    // The "Fechar" button is now added directly in gtk_dialog_new_with_buttons.
    // G_CALLBACK(on_help_dialog_close_clicked) is no longer needed as GTK_RESPONSE_CLOSE handles it.

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// =============================================================================
// MAIN APPLICATION FUNCTION
// =============================================================================

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    gtk_init(&argc, &argv);

    g_mkdir_with_parents("config", 0755);
    g_mkdir_with_parents("final", 0755);

    if (!load_categories_from_file("config/categories.txt")) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Erro: Não foi possível carregar as categorias do arquivo 'config/categories.txt'.\n"
                                                   "Por favor, crie este arquivo e adicione as categorias (uma por linha).");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 1;
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 800);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 10);

    GtkWidget *professor_frame = gtk_frame_new("Selecione o Professor");
    professor_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(GTK_WIDGET(professor_combo), -1, 40);
    gtk_widget_set_hexpand(GTK_WIDGET(professor_combo), TRUE);
    gtk_widget_set_halign(GTK_WIDGET(professor_combo), GTK_ALIGN_FILL);
    gtk_widget_set_vexpand(GTK_WIDGET(professor_combo), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(professor_combo), GTK_ALIGN_FILL);

    if (!load_professors_from_file("config/professores.txt")) {
    }
    gtk_container_add(GTK_CONTAINER(professor_frame), professor_combo);
    gtk_widget_set_hexpand(GTK_WIDGET(professor_frame), TRUE);
    gtk_widget_set_halign(GTK_WIDGET(professor_frame), GTK_ALIGN_FILL);
    gtk_widget_set_vexpand(GTK_WIDGET(professor_frame), FALSE);
    gtk_box_pack_start(GTK_BOX(main_vbox), professor_frame, FALSE, FALSE, 0);

    GtkWidget *top_buttons_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(main_vbox), top_buttons_hbox, FALSE, FALSE, 5);

    GtkWidget *config_btn = gtk_button_new_with_label("Configurações");
    g_signal_connect(config_btn, "clicked", G_CALLBACK(show_config_dialog), NULL);
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), config_btn, FALSE, FALSE, 0);

    GtkWidget *help_btn = gtk_button_new_with_label("Ajuda");
    g_signal_connect(help_btn, "clicked", G_CALLBACK(on_help_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), help_btn, FALSE, FALSE, 0);

    GtkWidget *reload_professor_btn = gtk_button_new_with_label("Recarregar Professor");
    g_signal_connect(reload_professor_btn, "clicked", G_CALLBACK(on_reload_professor_button_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), reload_professor_btn, FALSE, FALSE, 0);

    GtkWidget *create_prof_folder_btn = gtk_button_new_with_label("Criar Pasta do Professor");
    g_signal_connect(create_prof_folder_btn, "clicked", G_CALLBACK(on_create_professor_folder_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), create_prof_folder_btn, FALSE, FALSE, 0);

    g_signal_connect(professor_combo, "changed", G_CALLBACK(on_professor_selected), NULL);

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(main_vbox), hpaned, TRUE, TRUE, 0);

    GtkWidget *nav_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nav_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(nav_scrolled_window, 400, -1);
    gtk_paned_pack1(GTK_PANED(hpaned), nav_scrolled_window, FALSE, FALSE);

    category_nav_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(category_nav_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(nav_scrolled_window), category_nav_list_box);

    main_category_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(main_category_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_paned_pack2(GTK_PANED(hpaned), main_category_scrolled_window, TRUE, FALSE);

    category_content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(main_category_scrolled_window), category_content_vbox);
    gtk_container_set_border_width(GTK_CONTAINER(category_content_vbox), 5);

    create_category_gui(category_nav_list_box, category_content_vbox);

    GtkWidget *bottom_buttons_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(bottom_buttons_hbox, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(main_vbox), bottom_buttons_hbox, FALSE, FALSE, 10);

    save_all_btn = gtk_button_new_with_label("Salvar Alterações");
    g_signal_connect(save_all_btn, "clicked", G_CALLBACK(on_save_all_entries), NULL);
    gtk_box_pack_start(GTK_BOX(bottom_buttons_hbox), save_all_btn, TRUE, TRUE, 0);

    GtkWidget *generate_btn = gtk_button_new_with_label("Gerar Relatório Profissional");
    g_signal_connect(generate_btn, "clicked", G_CALLBACK(generate_report), NULL);
    gtk_box_pack_start(GTK_BOX(bottom_buttons_hbox), generate_btn, TRUE, TRUE, 0);

    preview_report_btn = gtk_button_new_with_label("Visualizar Relatório Profissional");
    g_signal_connect(preview_report_btn, "clicked", G_CALLBACK(on_preview_report_pdf), NULL);
    gtk_box_pack_start(GTK_BOX(bottom_buttons_hbox), preview_report_btn, TRUE, TRUE, 0);
    gtk_widget_hide(preview_report_btn);

    status_label = gtk_label_new("Pronto.");
    gtk_widget_set_hexpand(status_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
    gtk_box_pack_end(GTK_BOX(main_vbox), status_label, FALSE, FALSE, 5);

    progress_bar = gtk_progress_bar_new();
    gchar *initial_progress_text = g_strdup_printf("%d%%", 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), initial_progress_text);
    SAFE_FREE(initial_progress_text);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
    gtk_widget_set_hexpand(progress_bar, TRUE);
    gtk_box_pack_end(GTK_BOX(main_vbox), progress_bar, FALSE, FALSE, 5);

    gtk_widget_show_all(window);
    gtk_main();

    cleanup_category_gui();
    if (global_categories) {
        for (gint i = 0; i < global_num_categories; i++) {
            SAFE_FREE(global_categories[i]);
        }
        SAFE_FREE(global_categories);
    }
    SAFE_FREE(global_max_category_counters);

    return 0;
}

