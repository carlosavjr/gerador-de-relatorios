/**
 * @file report_gui.c
 * @brief GTK+ application for managing and generating professional PDF reports for professors.
 *
 * Improved version with:
 * - Better memory management
 * - Enhanced error handling
 * - Thread safety fixes (especially for external process spawning)
 * - More robust file operations
 * - NEW: Editable category field for each PDF entry.
 * - NEW: Filename generation based on counter, category, and year (NN_Category_YYYY.pdf).
 * - FIX: Resolved compilation warnings for incompatible pointer types.
 * - FIX: Resolved linker error by using gtk_entry_get_text instead of gtk_editable_get_text.
 * - FIX: Corrected folder structure to place professor directories directly in the application root.
 * - FIX: Adjusted "Select PDF" button size.
 * - FIX: Reverted "Preview" and "Remove" buttons to "Visualizar" and "Remover" text.
 * - UPDATE: Changed exiftool tag for category from 'Keywords' to 'Category'.
 * - FIX: Corrected typo in gtk_scrolled_window_new function name.
 * - FIX: Addressed unused parameter warnings by casting to void.
 * - FIX: Addressed implicit declaration of 'getline' by defining _GNU_SOURCE.
 * - FIX: Resolved incompatible function type cast for g_idle_add with a wrapper.
 * - FIX: Broke down long string literal in help dialog to conform to ISO C99.
 * - NEW: Added a button to explicitly create the professor's folder and its category subfolders.
 * - NEW: Added a warning dialog when trying to open a non-existent category folder.
 * - FIX: Added missing function definitions to resolve linker errors.
 * - FIX: Corrected argument count for `gtk_widget_get_allocation`.
 * - FIX: Corrected argument order for `gtk_message_dialog_new`.
 * - FIX: Implemented missing `on_file_set` function and reordered callback definitions.
 * - FIX: Removed redundant `!dir->d_name` check in `process_pdf_folder`.
 * - FIX: Implemented missing `on_open_category_folder` function.
 */

#define _GNU_SOURCE // Required for getline() on some systems

#include <gtk/gtk.h>       // Core GTK+ library for GUI elements.
#include <glib.h>          // GLib library for various utilities:
                           // GThread (for threading), GMutex, GCond, g_thread_new, g_idle_add.
#include <glib/gstdio.h>   // GLib I/O functions, specifically g_mkdir_with_parents.
#include <gio/gio.h>       // GIO library for file operations (GFile, GFileInfo) and GDesktopAppInfo.
#include <string.h>        // Standard C string manipulation functions (strcspn, strrchr).
#include <stdio.h>         // Standard C I/O functions (snprintf, popen, pclose, getline, fgets).
#include <errno.h>         // For checking error codes from system calls (e.g., EEXIST).
#include <dirent.h>        // For directory operations (opendir, readdir, closedir).
#include <sys/stat.h>      // For stat() (though GFile operations are preferred for cross-platform).
#include <locale.h>        // For setlocale, to handle character encoding correctly.
#include <stdlib.h>        // Standard C library for general utilities (atoi).
#include <sys/wait.h>      // For WIFEXITED, WEXITSTATUS (used with g_child_watch_add).
#include <limits.h>        // For PATH_MAX
#include <unistd.h>        // For close()

// =============================================================================
// STRUCTURE DEFINITIONS
// These structures define the data models used within the application.
// They are placed here to ensure they are defined before any global variables
// or function prototypes that use them.
// =============================================================================

/**
 * @brief Structure to hold information for each category section in the GUI.
 * Each category (e.g., "Comissões Examinadoras", "Produção Docente") has its
 * own section in the interface.
 */
typedef struct {
    GtkWidget *frame;   // The GtkFrame widget that visually groups the category's content.
    GtkWidget *list;    // GtkListBox to hold individual PdfEntry widgets (rows).
    GtkWidget *add_btn; // Button to add new PDF entries.
    GList *entries;     // A GLib GList storing pointers to PdfEntry structs
                        // associated with this category.
} CategorySection;

/**
 * @brief Structure to hold information for each individual PDF entry (row) in the GUI.
 * This bundles together UI widgets and associated data for a single PDF document.
 * @note Dynamic strings (gchar *) must be g_free'd.
 */
typedef struct {
    GtkWidget *box;         // Horizontal box containing all widgets for one PDF entry row.
    GtkWidget *file_btn;    // GtkFileChooserButton to select the PDF file.
    GtkWidget *counter_entry; // GtkEntry for manual counter input (e.g., "01", "10").
    GtkWidget *title_entry; // GtkEntry for the PDF's title metadata.
    GtkWidget *year_entry;  // GtkEntry for the PDF's year metadata.
    GtkWidget *category_entry; // NEW: GtkEntry for the PDF's category metadata.
    GtkWidget *remove_btn;  // Button to remove this PDF entry from the list.
    GtkWidget *preview_btn; // Button to open and preview the selected PDF.
    gchar *file_path;       // Dynamically allocated string: Stores the selected PDF's original file path.
    gchar *original_title;  // Dynamically allocated string: Stores the title as originally loaded/scanned.
    gchar *original_year;   // Dynamically allocated string: Stores the year as originally loaded/scanned.
    gchar *original_category; // Stores the category as originally loaded/scanned or derived.
    gchar *original_filename_no_ext; // Dynamically allocated string: Stores original filename without path/extension, for comparison during saving.
    gboolean has_original_counter;       // Flag: TRUE if the original file had a "NN_" counter prefix.
    gint original_detected_counter;      // The numeric value of the counter if found (e.g., 1 for "01_").
    gint current_counter_value; // Stores the current numeric value from `counter_entry` for sorting.
} PdfEntry;

/**
 * @brief Structure for asynchronous exiftool read operations.
 * This struct bundles all necessary data to be passed to and from the
 * asynchronous `exiftool` process, ensuring proper GUI updates on completion.
 * @note Dynamic strings (gchar *) must be g_free'd.
 */
typedef struct {
    gchar *file_path; // Dynamically allocated string: Path to the PDF file to be processed by exiftool.
    GtkListBoxRow *gui_row; // Pointer to the GtkListBoxRow whose PdfEntry needs updating after exiftool.
    gint category_index; // Index of the category for this PDF, used for updating max counters.
    gboolean is_new_file_selection; // Flag: TRUE if triggered by `on_file_set`, FALSE if by `process_pdf_folder`.
    gint stdout_fd; // File descriptor for exiftool's standard output.
    gint stderr_fd; // File descriptor for exiftool's standard error.
} ExiftoolReadOperation;


// =============================================================================
// GLOBAL VARIABLES
// These variables store application-wide data and references to key GUI widgets.
// =============================================================================

/**
 * @brief Stores the total number of categories loaded from 'config/categories.txt'.
 */
gint global_num_categories = 0;

/**
 * @brief An array of dynamically allocated strings, each representing a category
 * loaded from 'config/categories.txt'. This array is null-terminated.
 * @note This array and its contents must be freed at application exit.
 */
gchar **global_categories = NULL;

/**
 * @brief An array of `CategorySection` structs, dynamically allocated.
 * Each element represents a GUI section for a specific category, containing
 * its frame, listbox, and add button.
 * @note This array must be freed at application exit, but its internal GtkWidgets
 * are managed by GTK's destruction chain.
 */
CategorySection *global_sections = NULL;

/**
 * @brief A GtkComboBoxText widget used for selecting the current professor.
 */
GtkWidget *professor_combo;

/**
 * @brief A GtkListBox widget in the left pane, used for displaying category
 * shortcut buttons, allowing quick navigation to specific category sections.
 */
GtkWidget *category_nav_list_box;

/**
 * @brief A GtkBox (vertical) in the right pane, which acts as a container
 * for all individual category frames. This allows all categories to be
 * displayed in a single scrollable area.
 */
GtkWidget *category_content_vbox;

/**
 * @brief A GtkScrolledWindow that makes the `category_content_vbox` scrollable,
 * enabling users to view all category sections if they exceed the window height.
 */
GtkWidget *main_category_scrolled_window;

/**
 * @brief A GtkTextView widget used in the configuration dialog to edit
 * the list of professors.
 */
GtkWidget *professors_text_view = NULL;

/**
 * @brief A GtkTextView widget used in the configuration dialog to edit
 * the list of categories.
 */
GtkWidget *categories_text_view = NULL;

/**
 * @brief A GtkProgressBar widget used to visually indicate the progress
 * of long-running operations (e.g., loading professor data, generating reports).
 */
GtkWidget *progress_bar = NULL;

/**
 * @brief A GtkLabel widget used to display textual status updates to the user
 * during various application operations.
 */
GtkWidget *status_label = NULL;

/**
 * @brief A GtkButton widget to preview the generated professional report PDF.
 * This button is typically hidden until a report has been successfully generated.
 */
GtkWidget *preview_report_btn = NULL;

/**
 * @brief A GtkButton widget to save all current modifications made to PDF entries
 * without triggering a full report generation.
 */
GtkWidget *save_all_btn = NULL;

/**
 * @brief An array to store the maximum counter value found for each category.
 * This is used to suggest the next available counter for new entries or
 * for entries without an existing counter.
 * @note This array must be freed at application exit.
 */
gint *global_max_category_counters = NULL;

// Mutexs are removed as they are not needed for these global variables
// given the GTK main loop and GObject lifecycle management.

// =============================================================================
// FORWARD DECLARATIONS
// Declaring functions before their definitions allows for a more organized
// code structure and resolves dependencies.
// =============================================================================

// Callback functions for GUI events
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

// Helper functions for data management and GUI manipulation
gboolean load_categories_from_file(const gchar *filepath);
gboolean load_professors_from_file(const gchar *filepath);
void cleanup_category_gui();
void create_category_gui(GtkWidget *nav_list_box, GtkWidget *content_vbox);
void refresh_category_gui();
void refresh_professor_combo();
gchar* sanitize_filename(const gchar *input);
PdfEntry* create_pdf_entry();
GtkListBoxRow* create_pdf_entry_row(PdfEntry *entry, gint category_index); // Function to create a full row
void add_pdf_entry_to_gui(gint category_index, PdfEntry *entry, GtkListBoxRow *row);
gboolean save_single_pdf_entry(PdfEntry *entry, const gchar *professor_name, gint category_index);
void generate_report(GtkWidget *widget, gpointer data);
void process_pdf_folder(const gchar *folder_path, gint category_index);

// Configuration dialog functions
void save_professors_list(GtkWidget *button, gpointer user_data);
void save_categories_list(GtkWidget *button, gpointer user_data);
void show_config_dialog(GtkWidget *button, gpointer user_data);

// Help button callback
void on_help_button_clicked(GtkWidget *button, gpointer user_data);

// GDestroyNotify for PdfEntry
void pdf_entry_destroy_notify(gpointer data);

// Wrapper for on_professor_selected to match GSourceFunc signature for g_idle_add
gboolean on_professor_selected_idle_wrapper(gpointer user_data);


// =============================================================================
// HELPER FUNCTIONS
// (Definitions are placed here, after forward declarations, for proper linking)
// =============================================================================

/**
 * @brief Safely free a pointer and set it to NULL.
 * This macro helps prevent use-after-free errors by nullifying the pointer
 * immediately after freeing the memory it points to.
 * @param ptr The pointer to free and nullify.
 */
#define SAFE_FREE(ptr) do { if (ptr) { g_free(ptr); (ptr) = NULL; } } while (0)

/**
 * @brief Validates a file path for safety.
 * Checks for NULL, excessive length, and directory traversal attempts.
 * @param path The file path to validate.
 * @return TRUE if the path is considered valid, FALSE otherwise.
*/
gboolean is_valid_path(const gchar *path) {
    if (!path) return FALSE;
    // Check for path length (PATH_MAX is system-dependent)
    // Using >= because PATH_MAX includes the null terminator
    if (strlen(path) >= PATH_MAX) {
        g_warning("Path too long: %s", path);
        return FALSE;
    }
    // Prevent directory traversal attacks
    if (strstr(path, "../") != NULL || strstr(path, "/../") != NULL) {
        g_warning("Path contains directory traversal sequence: %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Callback function executed on the main GTK thread after an `exiftool`
 * process (spawned asynchronously) completes. This function safely reads its output,
 * parses metadata, and updates the GUI elements (`title_entry`, `year_entry`, `counter_entry`, `category_entry`).
 *
 * @param pid The process ID of the finished child process.
 * @param status The exit status of the child process.
 * @param user_data A pointer to the `ExiftoolReadOperation` struct containing the context.
 */
void exiftool_read_completed_callback(GPid pid, gint status, gpointer user_data) {
    ExiftoolReadOperation *op = (ExiftoolReadOperation *)user_data;
    if (!op) return;

    g_print("[Main Thread] Exiftool process (PID: %d) finished with status: %d\n", pid, status);

    // Clean up the child process resources
    g_spawn_close_pid(pid);

    // Read stdout and stderr from pipes
    gchar *stdout_buf = NULL;
    gchar *stderr_buf = NULL;
    gsize bytes_read;
    gchar buffer[4096]; // Buffer for reading pipe output

    GString *stdout_gstring = g_string_new("");
    GString *stderr_gstring = g_string_new("");

    // Read from stdout pipe
    while ((bytes_read = read(op->stdout_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        g_string_append(stdout_gstring, buffer);
    }
    stdout_buf = g_string_free(stdout_gstring, FALSE); // Get the string and free GString object

    // Read from stderr pipe
    while ((bytes_read = read(op->stderr_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        g_string_append(stderr_gstring, buffer);
    }
    stderr_buf = g_string_free(stderr_gstring, FALSE); // Get the string and free GString object

    // Close file descriptors
    close(op->stdout_fd);
    close(op->stderr_fd);

    // Retrieve the PdfEntry from the GtkListBoxRow.
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

    // Section: Initial title and counter extraction from filename
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

    // Section: Parse JSON output from exiftool
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && stdout_buf) {
        g_print("  [Main Thread] Exiftool JSON raw output: %s\n", stdout_buf);

        gchar *json_str = stdout_buf;
        gboolean title_found_by_exiftool = FALSE;
        gboolean year_found_by_exiftool = FALSE;
        gboolean category_found_by_exiftool = FALSE;

        // Manual JSON parsing for "Title"
        gchar *title_key = g_strstr_len(json_str, -1, "\"Title\":");
        if (title_key) {
            title_key += strlen("\"Title\":");
            gchar *title_start = strchr(title_key, '"');
            if (title_start) {
                title_start++;
                gchar *title_end = strchr(title_start, '"');
                if (title_end) {
                    SAFE_FREE(extracted_title_local); // Free filename-derived title if exiftool finds one.
                    extracted_title_local = g_strndup(title_start, title_end - title_start);
                    title_found_by_exiftool = TRUE;
                }
            }
        }

        // Manual JSON parsing for "Year"
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

        // Manual JSON parsing for "Category"
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

    // Section: Fallback for year from filename (if exiftool didn't find it)
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

    // NEW: Fallback for category - use the category of the section it was loaded into
    if (!extracted_category_local && op->category_index >= 0 && op->category_index < global_num_categories) {
        extracted_category_local = g_strdup(global_categories[op->category_index]);
    } else if (!extracted_category_local) {
        extracted_category_local = g_strdup("documento"); // Default if no category found
    }


    // Section: Update GUI Entry Fields
    gtk_entry_set_text(GTK_ENTRY(entry->title_entry), extracted_title_local ? extracted_title_local : "Título Não Informado");
    gtk_entry_set_text(GTK_ENTRY(entry->year_entry), extracted_year_local ? extracted_year_local : "");
    gtk_entry_set_text(GTK_ENTRY(entry->category_entry), extracted_category_local);

    // Section: Update Original Fields in PdfEntry Struct
    SAFE_FREE(entry->original_title);
    entry->original_title = g_strdup(extracted_title_local ? extracted_title_local : "Título Não Informado");
    SAFE_FREE(entry->original_year);
    entry->original_year = g_strdup(extracted_year_local ? extracted_year_local : "");
    SAFE_FREE(entry->original_category);
    entry->original_category = g_strdup(extracted_category_local);
    SAFE_FREE(entry->original_filename_no_ext);
    entry->original_filename_no_ext = original_filename_no_ext_local;
    entry->current_counter_value = entry->original_detected_counter;

    // Section: Update Max Category Counter (for `process_pdf_folder` context)
    if (!op->is_new_file_selection && entry->original_detected_counter > 0) {
        if (entry->original_detected_counter > global_max_category_counters[op->category_index]) {
            global_max_category_counters[op->category_index] = entry->original_detected_counter;
        }
    }
    gchar *counter_text = g_strdup_printf("%02d", entry->original_detected_counter);
    gtk_entry_set_text(GTK_ENTRY(entry->counter_entry), counter_text);
    SAFE_FREE(counter_text);

    // Section: Trigger Listbox Re-sort
    GtkWidget *list_box = gtk_widget_get_parent(GTK_WIDGET(op->gui_row));
    if (list_box && GTK_IS_LIST_BOX(list_box)) {
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(list_box));
    }

    // Section: Update Progress Bar
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
    // Section: Free Allocated Strings within Operation Struct
    SAFE_FREE(extracted_title_local);
    SAFE_FREE(extracted_year_local);
    SAFE_FREE(extracted_category_local);
    SAFE_FREE(stdout_buf);
    SAFE_FREE(stderr_buf);
    SAFE_FREE(op->file_path);
    SAFE_FREE(op);
}

/**
 * @brief Callback function for when a file is selected via the GtkFileChooserButton.
 * This function is triggered when the user picks a PDF file. It updates the
 * `PdfEntry` struct with the selected file path and initiates an asynchronous
 * `exiftool` process to extract metadata (title, year, category) from the PDF.
 * The GUI is updated with placeholder text until the metadata is available.
 *
 * @param button The GtkFileChooserButton that emitted the "file-set" signal.
 * @param row The GtkListBoxRow to which the PdfEntry is attached.
 */
void on_file_set(GtkFileChooserButton *button, GtkListBoxRow *row) {
    g_print("File set callback triggered.\n");

    // Retrieve the PdfEntry struct from the GtkListBoxRow's data.
    PdfEntry *entry = g_object_get_data(G_OBJECT(row), "pdf-entry");
    if (!entry) {
        g_warning("PdfEntry data not found for the row. Cannot process file selection.");
        return;
    }

    // Get the selected file's full path.
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

    // Update the PdfEntry's file_path. Free the old one if it exists.
    SAFE_FREE(entry->file_path);
    entry->file_path = g_strdup(file_path);
    SAFE_FREE(file_path); // Free the GtkFileChooser allocated string.

    // Update the GUI entries with "Loading..." placeholders.
    gtk_entry_set_text(GTK_ENTRY(entry->title_entry), "Carregando título...");
    gtk_entry_set_text(GTK_ENTRY(entry->year_entry), "Carregando ano...");
    gtk_entry_set_text(GTK_ENTRY(entry->category_entry), "Carregando categoria...");

    // Determine the category index for the PdfEntry.
    // This assumes the row is directly in a CategorySection's listbox.
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
        // We can still proceed with exiftool, but max counter update won't work correctly.
    }


    // Create an ExiftoolReadOperation struct for the asynchronous task.
    ExiftoolReadOperation *op = g_malloc0(sizeof(ExiftoolReadOperation));
    op->file_path = g_strdup(entry->file_path); // Duplicate path for the child process.
    op->gui_row = row;                           // Pass the GtkListBoxRow to update.
    op->category_index = category_index;         // Pass the determined category index.
    op->is_new_file_selection = TRUE;            // Flag indicating this is a new file selection.

    // Construct the exiftool command to extract JSON metadata for Title, Year, and Category.
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
    // Spawn exiftool process asynchronously, capturing its stdout and stderr.
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

    // Add a child watch to call `exiftool_read_completed_callback` when exiftool finishes.
    g_child_watch_add(pid, exiftool_read_completed_callback, op);
}

/**
 * @brief Callback for when the text in a `PdfEntry`'s counter entry changes.
 * This function updates the internal `current_counter_value` of the `PdfEntry`
 * and triggers a re-sort of the `GtkListBox` to reflect the new order.
 *
 * @param editable The GtkEditable (which is the GtkEntry for the counter) that emitted the "changed" signal.
 * @param entry The `PdfEntry` struct associated with this counter entry.
 */
void on_counter_entry_changed(GtkEditable *editable, PdfEntry *entry) {
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(editable)); // Get the current text from the entry.
    gint new_val = atoi(text); // Convert the text to an integer.
    if (new_val < 0) new_val = 0; // Ensure the counter value is non-negative.

    // Only update and re-sort if the counter value has actually changed.
    if (entry->current_counter_value != new_val) {
        entry->current_counter_value = new_val; // Update the internal counter value.

        // Find the parent `GtkListBox` to invalidate its sort.
        GtkWidget *parent_row = gtk_widget_get_parent(entry->box); // Get the `GtkListBoxRow` containing the entry's box.
        if (parent_row && GTK_IS_LIST_BOX_ROW(parent_row)) {
            GtkWidget *list_box = gtk_widget_get_parent(parent_row); // Get the `GtkListBox` containing the row.
            if (list_box && GTK_IS_LIST_BOX(list_box)) {
                gtk_list_box_invalidate_sort(GTK_LIST_BOX(list_box)); // Trigger a re-sort.
            }
        }
    }
}

/**
 * @brief Callback for the "changed" signal of the category entry.
 * Updates the PdfEntry's internal category string.
 * @param editable The GtkEditable (category entry) that emitted the signal.
 * @param entry The PdfEntry struct associated with the entry.
 */
void on_category_entry_changed(GtkEditable *editable, PdfEntry *entry) {
    // FIX: Use gtk_entry_get_text for GtkEntry widgets
    const gchar *new_category = gtk_entry_get_text(GTK_ENTRY(editable));
    g_print("Category changed for %s to: %s\n", entry->file_path ? entry->file_path : "N/A", new_category);
    SAFE_FREE(entry->original_category); // Free old string
    entry->original_category = g_strdup(new_category); // Store new string
    // If sorting by category were desired, gtk_list_box_invalidate_sort could be called here.
}


/**
 * @brief Comparison function for sorting PdfEntry structs by their `current_counter_value`.
 * This function is used by GtkListBox to sort rows based on the numeric value in the
 * counter entry.
 *
 * @param row1 The first GtkListBoxRow to compare.
 * @param row2 The second GtkListBoxRow to compare.
 * @param user_data User data (not used in this specific comparison, but required by signature).
 * @return A negative integer if `entry_a`'s counter is less than `entry_b`'s,
 * zero if they are equal, or a positive integer if `entry_a`'s counter is greater.
 */
gint compare_pdf_entries_by_counter(GtkListBoxRow *row1, GtkListBoxRow *row2, gpointer user_data) {
    (void)user_data; // Suppress unused parameter warning

    // Retrieve the PdfEntry struct from the GtkListBoxRow's data.
    // The PdfEntry was attached to the row using g_object_set_data_full.
    PdfEntry *entry_a = g_object_get_data(G_OBJECT(row1), "pdf-entry");
    PdfEntry *entry_b = g_object_get_data(G_OBJECT(row2), "pdf-entry");

    if (!entry_a || !entry_b) return 0; // Defensive check

    // Get the current counter value from the stored `current_counter_value` member.
    gint counter_a = entry_a->current_counter_value;
    gint counter_b = entry_b->current_counter_value;

    // Sort in ascending order (smaller counter values come first).
    return counter_a - counter_b;
}

/**
 * @brief GDestroyNotify function for PdfEntry.
 * This function is registered with `g_object_set_data_full` and is called
 * automatically by GTK when the GtkListBoxRow (to which the PdfEntry is attached)
 * is destroyed. Its purpose is to free all dynamically allocated memory within
 * the PdfEntry struct, preventing memory leaks.
 *
 * @param data A pointer to the PdfEntry struct that needs to be freed.
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
 * @brief Sanitizes a string to be suitable for use as a filename.
 * Preserves accented characters but replaces problematic filename characters and spaces with underscores.
 *
 * @param input The original string to sanitize.
 * @return A newly allocated, sanitized string. Must be freed with SAFE_FREE().
 */
gchar* sanitize_filename(const gchar *input) {
    if (!input) return g_strdup("documento_sem_titulo");

    GString *clean_string = g_string_new("");
    const gchar *p = input;
    gunichar c;

    while (*p) {
        c = g_utf8_get_char(p); // Get the UTF-8 character

        // Check for common problematic characters in filenames
        // This list can be expanded based on OS restrictions (e.g., Windows: / \ : * ? " < > |)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '\"' || c == '<' || c == '>' || c == '|' || c == '\0') {
            g_string_append_unichar(clean_string, '_');
        } else if (g_unichar_isspace(c)) { // Replace any whitespace with underscore
            g_string_append_unichar(clean_string, '_');
        } else {
            // Append the character as is, including accents
            g_string_append_unichar(clean_string, c);
        }
        p = g_utf8_next_char(p); // Move to the next UTF-8 character
    }

    // Now, handle consecutive underscores and remove leading/trailing ones
    gchar *temp_sanitized = g_string_free(clean_string, FALSE); // Get the string and free GString object

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

    SAFE_FREE(temp_sanitized); // Free the intermediate string

    // Remove leading/trailing underscores (if any remain after consolidation)
    gchar *result = g_string_free(final_sanitized, FALSE);
    gchar *trimmed_result = g_strstrip(result); // This modifies in place, removing leading/trailing whitespace/underscores

    // If after sanitization it's empty, use a default
    if (g_strcmp0(trimmed_result, "") == 0) {
        SAFE_FREE(result); // Free the original allocated string
        return g_strdup("documento_sem_titulo");
    }

    // g_strstrip modifies in place, so result now points to the trimmed string
    return result; // Return the modified string
}

/**
 * @brief Creates a new PdfEntry struct and initializes its members.
 * @return A pointer to a newly allocated PdfEntry struct. Must be freed when no longer needed.
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
 * @brief Creates a GtkListBoxRow containing widgets for a PdfEntry.
 * This function sets up the GUI representation for a single PDF document.
 *
 * @param entry A pointer to the PdfEntry struct for which the row is being created.
 * @param category_index The index of the category this PDF belongs to.
 * @return The newly created GtkListBoxRow.
 */
GtkListBoxRow* create_pdf_entry_row(PdfEntry *entry, gint category_index) {
    (void)category_index; // Suppress unused parameter warning

    GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
    entry->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5); // 5px spacing

    // Store PdfEntry data on the row, with a destroy notify to free it
    g_object_set_data_full(G_OBJECT(row), "pdf-entry", entry, pdf_entry_destroy_notify);

    // File Chooser Button
    entry->file_btn = gtk_file_chooser_button_new("Selecionar PDF", GTK_FILE_CHOOSER_ACTION_OPEN);
    // FIX: Set a fixed size for the file chooser button
    gtk_widget_set_size_request(entry->file_btn, 150, -1); // Set width to 150px, height automatic
    gtk_box_pack_start(GTK_BOX(entry->box), entry->file_btn, FALSE, FALSE, 0); // Don't expand
    g_signal_connect(G_OBJECT(entry->file_btn), "file-set", G_CALLBACK(on_file_set), row);

    // Counter Entry (e.g., "01")
    entry->counter_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry->counter_entry), 3); // For "01" or "10"
    gtk_entry_set_max_length(GTK_ENTRY(entry->counter_entry), 2);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->counter_entry), "NN");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->counter_entry, FALSE, FALSE, 5);
    g_signal_connect(G_OBJECT(entry->counter_entry), "changed", G_CALLBACK(on_counter_entry_changed), entry);

    // Title Entry
    entry->title_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->title_entry), "Título do PDF");
    gtk_widget_set_hexpand(entry->title_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(entry->box), entry->title_entry, TRUE, TRUE, 0);

    // Year Entry
    entry->year_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry->year_entry), 5); // For "2024"
    gtk_entry_set_max_length(GTK_ENTRY(entry->year_entry), 4);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->year_entry), "Ano");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->year_entry, FALSE, FALSE, 5);

    // NEW: Category Entry
    entry->category_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry->category_entry), 15); // Example width
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->category_entry), "Categoria (ex: artigo)");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->category_entry, FALSE, FALSE, 5);
    g_signal_connect(G_OBJECT(entry->category_entry), "changed", G_CALLBACK(on_category_entry_changed), entry);


    // Preview Button
    // FIX: Changed button label to "Visualizar"
    entry->preview_btn = gtk_button_new_with_label("Visualizar");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->preview_btn, FALSE, FALSE, 5);
    g_signal_connect(G_OBJECT(entry->preview_btn), "clicked", G_CALLBACK(on_preview_pdf), entry);

    // Remove Button
    // FIX: Changed button to use label "Remover" instead of icon
    entry->remove_btn = gtk_button_new_with_label("Remover");
    gtk_box_pack_start(GTK_BOX(entry->box), entry->remove_btn, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(entry->remove_btn), "clicked", G_CALLBACK(on_remove_pdf_entry), entry);

    gtk_container_add(GTK_CONTAINER(row), entry->box);
    gtk_widget_show_all(entry->box); // Show all widgets inside the box

    return row;
}


/**
 * @brief Adds a PdfEntry to the GUI, creating its row and appending it to the correct listbox.
 * @param category_index The index of the category section where the entry should be added.
 * @param entry A pointer to the PdfEntry struct to add.
 * @param row The GtkListBoxRow corresponding to this entry.
 */
void add_pdf_entry_to_gui(gint category_index, PdfEntry *entry, GtkListBoxRow *row) {
    if (category_index >= 0 && category_index < global_num_categories) {
        // FIX: Cast row to GtkWidget* as expected by gtk_list_box_insert
        gtk_list_box_insert(GTK_LIST_BOX(global_sections[category_index].list), GTK_WIDGET(row), -1); // Add to the end
        // Add to the GList that holds entries for this category
        global_sections[category_index].entries = g_list_append(global_sections[category_index].entries, entry);
        // FIX: Cast row to GtkWidget* as expected by gtk_widget_show_all
        gtk_widget_show_all(GTK_WIDGET(row)); // Make sure the new row and its children are visible
    } else {
        g_warning("Invalid category index %d for adding PDF entry to GUI.", category_index);
        // The row and entry should be freed here if they were newly created and not added.
        // For simplicity, assuming caller handles if this function fails to insert.
    }
}

/**
 * @brief Loads categories from a specified text file, with one category per line.
 * This function populates the `global_num_categories` count and the
 * `global_categories` array, which are used to dynamically create GUI sections.
 *
 * @param filepath The full path to the categories configuration file (e.g., "config/categories.txt").
 * @return TRUE if categories were loaded successfully, FALSE otherwise (e.g., file not found, empty file).
 */
gboolean load_categories_from_file(const gchar *filepath) {
    // Section: Cleanup existing categories
    // Free any previously loaded categories to prevent memory leaks before loading new ones.
    if (global_categories) {
        for (gint i = 0; i < global_num_categories; i++) {
            SAFE_FREE(global_categories[i]);
        }
        SAFE_FREE(global_categories);
    }
    global_num_categories = 0; // Reset category count.

    GList *category_list = NULL; // Temporary GLib list to store categories as they are read.
    gchar *line = NULL;          // Buffer for reading each line from the file.
    gsize len = 0;               // Size of the line buffer.
    gssize read;                 // Number of characters read by getline.
    FILE *fp = fopen(filepath, "r"); // Open the categories file for reading.

    // Section: File opening error handling
    if (!fp) {
        g_warning("Could not open categories file '%s': %s", filepath, g_strerror(errno));
        return FALSE; // Return FALSE if the file cannot be opened.
    }

    // Section: Read file line by line
    while ((read = getline(&line, &len, fp)) != -1) {
        line[strcspn(line, "\n")] = 0; // Remove newline character from the end of the line.
        if (g_strcmp0(line, "") != 0) { // Skip empty lines.
            category_list = g_list_append(category_list, g_strdup(line)); // Duplicate and append non-empty lines.
        }
    }
    fclose(fp); // Close the file.
    SAFE_FREE(line); // Free the buffer allocated by getline.

    // Section: Populate global_categories array
    global_num_categories = g_list_length(category_list); // Get the total number of categories.
    if (global_num_categories == 0) {
        g_warning("No categories found in '%s'. Please add categories, one per line.", filepath);
        g_list_free_full(category_list, g_free); // Free all allocated strings in the list.
        return FALSE; // Return FALSE if no categories were found.
    }

    // Allocate memory for the global_categories array of char pointers.
    global_categories = g_new0(gchar*, global_num_categories + 1); // +1 for NULL terminator.
    gint i = 0;
    // Iterate through the temporary GList and copy pointers to the global array.
    for (GList *l = category_list; l != NULL; l = l->next) {
        global_categories[i++] = (gchar*)l->data; // Transfer ownership of duplicated strings.
    }
    global_categories[i] = NULL; // Null-terminate the array for easier iteration.

    g_list_free(category_list); // Free the GList structure itself (elements are now owned by global_categories).

    g_print("Loaded %d categories from '%s'.\n", global_num_categories, filepath);
    return TRUE; // Return TRUE on successful loading.
}

/**
 * @brief Loads professor names from a specified text file, one professor per line.
 * This function populates the `professor_combo` GtkComboBoxText widget with the loaded names.
 *
 * @param filepath The full path to the professors configuration file (e.g., "config/professores.txt").
 * @return TRUE if professors were loaded successfully, FALSE otherwise.
 */
gboolean load_professors_from_file(const gchar *filepath) {
    // Clear all existing entries from the combo box before loading new ones.
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(professor_combo));

    FILE *fp = fopen(filepath, "r"); // Open the professors file for reading.
    if(fp) {
        char line[256]; // Buffer for reading each line.
        // Read file line by line using fgets.
        while(fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = 0; // Remove newline character.
            if (g_strcmp0(line, "") != 0) { // Skip empty lines.
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(professor_combo), line); // Add professor name to combo box.
            }
        }
        fclose(fp); // Close the file.
        return TRUE; // Return TRUE on successful loading.
    } else {
        // Error handling if the file cannot be opened.
        g_warning("Could not open 'config/professores.txt'. Please ensure the file exists.");
        // Add a placeholder text to the combo box if no professors are available.
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(professor_combo), "Nenhum Professor Disponível");
        return FALSE; // Return FALSE on failure.
    }
}

/**
 * @brief Saves the content of the `professors_text_view` (from the config dialog)
 * to the `config/professores.txt` file. After saving, it also ensures that
 * corresponding professor directories and their category subdirectories exist.
 *
 * @param button The GtkButton that triggered this save operation.
 * @param user_data User data (not used in this function).
 */
void save_professors_list(GtkWidget *button, gpointer user_data) {
    (void)button;    // Suppress unused parameter warning
    (void)user_data; // Suppress unused parameter warning

    // Get the text buffer from the professors_text_view.
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(professors_text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    // Extract the entire text content from the buffer.
    gchar *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    GError *error = NULL;
    // Attempt to save the content to the professors.txt file.
    if (g_file_set_contents("config/professores.txt", content, -1, &error)) {
        g_print("Professors list saved. Now creating directories...\n");
        // FIX: Changed professor_base_dir to "./" as per user's specified structure
        gchar *professor_base_dir = g_strdup("./"); // Base directory for professor folders.

        // Parse the content line by line to get each professor's name
        gchar **professors_array = g_strsplit(content, "\n", 0);
        for (gint p_idx = 0; professors_array[p_idx] != NULL; p_idx++) {
            gchar *professor_name = g_strstrip(professors_array[p_idx]); // Get and strip whitespace from professor name.
            if (g_strcmp0(professor_name, "") == 0) continue; // Skip empty lines.

            g_print("  Processing professor: %s\n", professor_name);

            // Create professor's main directory (e.g., "./Professor A").
            gchar *professor_folder_path = g_strdup_printf("%s%s", professor_base_dir, professor_name);
            // `g_mkdir_with_parents` creates all necessary parent directories.
            if (!g_mkdir_with_parents(professor_folder_path, 0755)) { // 0755 permissions: rwx for owner, rx for group/others.
                if (errno != EEXIST) { // Check if the error is not "already exists".
                    g_warning("    Failed to create professor directory '%s': %s", professor_folder_path, g_strerror(errno));
                } else {
                    g_print("    Professor directory already exists: %s\n", professor_folder_path);
                }
            } else {
                g_print("    Professor directory created: %s\n", professor_folder_path);
            }

            // FIX: Validate global_num_categories before accessing global_categories
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
                SAFE_FREE(category_folder_path); // Free category folder path.
            }
            SAFE_FREE(professor_folder_path); // Free professor folder path.
        }
        g_strfreev(professors_array); // Free the array of professor name strings.
        SAFE_FREE(professor_base_dir);   // Free base directory string.


        // Show success message and refresh the main window's professor combo box.
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Lista de professores salva com sucesso!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        refresh_professor_combo(); // Update the main GUI's professor dropdown.
    } else {
        // Error handling if saving the file fails.
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Erro ao salvar lista de professores: %s", error ? error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error); // Free the error object.
    }
    SAFE_FREE(content); // Free the content string.
}

/**
 * @brief Saves the content of the `categories_text_view` (from the config dialog)
 * to the `config/categories.txt` file.
 *
 * @param button The GtkButton that triggered this save operation.
 * @param user_data User data (not used in this function).
 */
void save_categories_list(GtkWidget *button, gpointer user_data) {
    (void)button;    // Suppress unused parameter warning
    (void)user_data; // Suppress unused parameter warning

    // Get the text buffer from the categories_text_view.
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(categories_text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    // Extract the entire text content from the buffer.
    gchar *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    GError *error = NULL;
    // Attempt to save the content to the categories.txt file.
    if (g_file_set_contents("config/categories.txt", content, -1, &error)) {
        // Show success message and refresh the main window's category GUI.
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Lista de categorias salva com sucesso!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        refresh_category_gui(); // Update the main GUI's category sections.
    } else {
        // Error handling if saving the file fails.
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Erro ao salvar lista de categorias: %s", error ? error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error); // Free the error object.
    }
    SAFE_FREE(content); // Free the content string.
}

// =============================================================================
// GUI MANAGEMENT FUNCTIONS
// Functions responsible for creating, updating, and cleaning up GUI elements.
// =============================================================================

/**
 * @brief Cleans up (destroys) all existing category GUI elements and frees associated memory.
 * This is crucial before reloading categories or changing professors to avoid memory leaks
 * and ensure a fresh GUI state.
 */
void cleanup_category_gui() {
    // Section: Destroy category frames and their contents
    if (global_sections) {
        for (gint i = 0; i < global_num_categories; i++) {
            // Destroy the GtkFrame for each category. This will recursively destroy
            // all its child widgets (GtkListBox, GtkListBoxRows, etc.) and
            // trigger any associated GDestroyNotify callbacks (like `pdf_entry_destroy_notify`).
            if (global_sections[i].frame) {
                gtk_widget_destroy(global_sections[i].frame);
            }
            // After destroying the widgets, explicitly free the GList structure itself.
            // The PdfEntry data pointed to by the GList elements will be freed by
            // `pdf_entry_destroy_notify` when their respective GtkListBoxRows are destroyed.
            if (global_sections[i].entries) {
                g_list_free(global_sections[i].entries);
                global_sections[i].entries = NULL;
            }
        }
        SAFE_FREE(global_sections); // Free the array of CategorySection structs.
    }

    // Section: Clear the main category content vbox
    // Remove all children from the main `category_content_vbox`.
    // This ensures no lingering widgets from previous GUI states remain.
    if (category_content_vbox) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(category_content_vbox));
        for (GList *l = children; l != NULL; l = g_list_next(l)) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children); // Free the GList structure that held the children pointers.
    }

    // Section: Clear the category navigation listbox
    // Remove all children (shortcut buttons) from the `category_nav_list_box`.
    if (category_nav_list_box) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(category_nav_list_box));
        for (GList *l = children; l != NULL; l = l->next) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children); // Free the GList structure.
    }
}

/**
 * @brief Creates and populates the category GUI elements based on the `global_categories` array.
 * This function dynamically generates GtkFrames, GtkListBoxes, and buttons for each category.
 * It also creates shortcut buttons in the navigation pane.
 *
 * @param nav_list_box The GtkListBox in the left pane where category shortcut buttons will be added.
 * @param content_vbox The GtkBox in the right pane where category frames will be packed.
 */
void create_category_gui(GtkWidget *nav_list_box, GtkWidget *content_vbox) {
    // Allocate the `global_sections` array based on the number of loaded categories.
    global_sections = g_new0(CategorySection, global_num_categories);

    // Loop through each category to create its GUI section.
    for(gint i = 0; i < global_num_categories; i++) {
        // Section: Create Category Frame and its internal layout
        global_sections[i].frame = gtk_frame_new(global_categories[i]); // Create a frame with the category name as label.
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); // Vertical box for content within the frame.

        global_sections[i].list = gtk_list_box_new(); // Create a list box to hold PDF entries.
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(global_sections[i].list), GTK_SELECTION_NONE); // Disable selection.
        gtk_widget_set_hexpand(GTK_WIDGET(global_sections[i].list), TRUE); // Allow list to expand horizontally.
        gtk_widget_set_halign(GTK_WIDGET(global_sections[i].list), GTK_ALIGN_FILL); // Fill available horizontal space.

        // Create a horizontal box for "Add PDF" and "Open Folder" buttons.
        GtkWidget *buttons_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_widget_set_halign(buttons_hbox, GTK_ALIGN_END); // Align buttons to the right.

        // Section: Add PDF Button
        global_sections[i].add_btn = gtk_button_new_with_label("Adicionar PDF");
        // Connect the "clicked" signal to `add_pdf_entry` callback, passing the current section.
        g_signal_connect(global_sections[i].add_btn, "clicked", G_CALLBACK(add_pdf_entry), &global_sections[i]);
        gtk_box_pack_end(GTK_BOX(buttons_hbox), global_sections[i].add_btn, FALSE, FALSE, 0);

        // Section: Open Folder Button
        GtkWidget *open_folder_btn = gtk_button_new_with_label("Abrir Pasta");
        // Connect the "clicked" signal to `on_open_category_folder`, passing the category index.
        g_signal_connect(open_folder_btn, "clicked", G_CALLBACK(on_open_category_folder), GINT_TO_POINTER(i));
        gtk_box_pack_end(GTK_BOX(buttons_hbox), open_folder_btn, FALSE, FALSE, 0);


        GtkWidget *list_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
        // Set scrollbar policy to automatic (show only when content overflows).
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scrolled_window),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_hexpand(list_scrolled_window, TRUE);
        gtk_widget_set_halign(list_scrolled_window, GTK_ALIGN_FILL);
        gtk_widget_set_size_request(list_scrolled_window, -1, 300); // Set a minimum height for the list.
        gtk_container_add(GTK_CONTAINER(list_scrolled_window), global_sections[i].list);

        // Pack the scrolled window and buttons into the category's vertical box.
        gtk_box_pack_start(GTK_BOX(box), list_scrolled_window, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), buttons_hbox, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(global_sections[i].frame), box); // Add the vertical box to the frame.

        // Section: Pack Category Frame into Main Content Area
        // Pack the category frame into the main `category_content_vbox` (right pane).
        gtk_box_pack_start(GTK_BOX(content_vbox), global_sections[i].frame, TRUE, TRUE, 30); // Allow frame to expand, add spacing.
        gtk_widget_set_hexpand(GTK_WIDGET(global_sections[i].frame), TRUE); // Frame expands horizontally.
        gtk_widget_set_halign(GTK_WIDGET(global_sections[i].frame), GTK_ALIGN_FILL); // Fill horizontal space.
        gtk_widget_set_vexpand(GTK_WIDGET(global_sections[i].frame), TRUE); // Frame expands vertically.

        // Section: Create Category Shortcut Button for Navigation Pane
        GtkWidget *shortcut_btn = gtk_button_new_with_label(global_categories[i]); // Button with category name.
        // Store a reference to the actual category frame in the button's data.
        // This allows the `on_category_shortcut_clicked` callback to easily find the target frame.
        g_object_set_data(G_OBJECT(shortcut_btn), "category-frame", global_sections[i].frame);
        g_signal_connect(shortcut_btn, "clicked", G_CALLBACK(on_category_shortcut_clicked), NULL);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), shortcut_btn);
        gtk_list_box_insert(GTK_LIST_BOX(nav_list_box), GTK_WIDGET(row), -1); // FIX: Cast to GtkWidget*
    }
    // Show all newly created widgets in both navigation and content panes.
    gtk_widget_show_all(nav_list_box);
    gtk_widget_show_all(content_vbox);
}

/**
 * @brief Refreshes the entire category GUI by first cleaning up existing elements
 * and then recreating them based on the current `config/categories.txt` file.
 * This is typically called after changes are made in the configuration dialog.
 */
void refresh_category_gui() {
    g_print("Refreshing category GUI...\n");
    cleanup_category_gui(); // Destroy all old category widgets and free associated memory.
    // Reload categories from file and recreate the GUI if successful.
    if (load_categories_from_file("config/categories.txt")) {
        create_category_gui(category_nav_list_box, category_content_vbox); // Recreate the GUI.
    } else {
        g_warning("Failed to refresh categories after loading from file.");
    }
    // Clear the professor selection as the category structure might have changed,
    // which would invalidate the current professor's loaded data.
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Refreshes the professor combo box by reloading the list of professors
 * from the `config/professores.txt` file. This is typically called after changes
 * are made in the configuration dialog.
 */
void refresh_professor_combo() {
    g_print("Refreshing professor combo...\n");
    // Reload professors from file. A warning is logged by the function if loading fails.
    if (!load_professors_from_file("config/professores.txt")) {
        g_warning("Failed to refresh professors after loading from file.");
    }
    // Clear the current selection to ensure that `on_professor_selected` is triggered
    // if the same professor is re-selected, or if a new one is chosen.
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Callback for the "Adicionar PDF" button.
 * This function creates a new, blank `PdfEntry` and adds it to the GUI
 * in the appropriate category section. It first checks if a professor is selected.
 *
 * @param widget The GtkButton that emitted the "clicked" signal.
 * @param section A pointer to the `CategorySection` struct to which the new PDF entry should be added.
 */
void add_pdf_entry(GtkWidget *widget, CategorySection *section) {
    (void)widget; // Suppress unused parameter warning

    // Get the currently selected professor's name from the combo box.
    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));

    // Input validation: Check if a professor has been selected.
    if (!professor || g_strcmp0(professor, "") == 0) {
        // Display an error dialog if no professor is selected.
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Por favor, selecione um professor antes de adicionar um PDF.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return; // Exit the function if validation fails.
    }

    PdfEntry *entry = create_pdf_entry(); // Create a new PdfEntry struct and its widgets.
    // For manually added entries, there's no original counter from a filename.
    entry->has_original_counter = FALSE;
    entry->original_detected_counter = 0;
    entry->current_counter_value = 0; // Initialize current counter value to 0.
    entry->original_filename_no_ext = g_strdup(""); // Set to empty for new entries.
    entry->original_category = g_strdup(global_categories[section - global_sections]); // Default category to section name

    GtkListBoxRow *row = create_pdf_entry_row(entry, section - global_sections);

    // Add the newly created PdfEntry to the GUI.
    // The `section - global_sections` calculation determines the category index.
    add_pdf_entry_to_gui(section - global_sections, entry, row);
}


/**
 * @brief Callback function for the "Remover" button on a `PdfEntry`.
 * This function removes the `PdfEntry` from the GUI, frees its associated memory,
 * and attempts to move the original PDF file to an 'old_files' subfolder within
 * the corresponding category directory.
 *
 * @param button The GtkButton that emitted the "clicked" signal.
 * @param entry The `PdfEntry` struct associated with the button that was clicked.
 */
void on_remove_pdf_entry(GtkWidget *button, PdfEntry *entry) {
    (void)button; // Suppress unused parameter warning

    g_print("Removing PDF entry: %s\n", entry->file_path ? entry->file_path : "N/A");

    GtkWidget *row = NULL;
    GtkWidget *list_box = NULL;
    CategorySection *current_section = NULL;
    gint category_index = -1;
    const gchar *professor = NULL;

    // Section: Get GUI context first and validate pointers
    // Traverse up the widget hierarchy to find the parent GtkListBoxRow and GtkListBox.
    row = gtk_widget_get_parent(entry->box);
    if (row && GTK_IS_LIST_BOX_ROW(row)) {
        list_box = gtk_widget_get_parent(row);
        if (list_box && GTK_IS_LIST_BOX(list_box)) {
            // Find the `CategorySection` corresponding to this `GtkListBox`.
            for (gint i = 0; i < global_num_categories; i++) {
                if (global_sections[i].list == list_box) {
                    current_section = &global_sections[i];
                    category_index = i;
                    break;
                }
            }
        }
    }

    // 2. Get professor selection (can be NULL)
    professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(professor_combo));

    // --- File System Operation: Move to old_files ---
    // Only attempt to move if professor is selected AND file_path is valid AND file exists AND GUI context is valid
    if (professor && entry->file_path && is_valid_path(entry->file_path) && g_file_test(entry->file_path, G_FILE_TEST_EXISTS) && current_section) {
        // FIX: Changed professor_base_dir to "./" as per user's specified structure
        gchar *professor_base_dir = g_strdup("./"); // Base directory for professor folders.
        // Use the original professor name for path, NOT sanitized
        gchar *dest_category_dir = g_strdup_printf("%s%s/%s", professor_base_dir, professor, global_categories[category_index]);
        // Construct the 'old_files' directory inside the category directory
        gchar *old_files_dir = g_strdup_printf("%s/old_files", dest_category_dir);

        g_print("  Attempting to ensure old_files directory for removal exists: %s\n", old_files_dir);
        // Create the 'old_files' directory if it's not already there
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

        // Perform the move operation
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

    // --- GUI Removal and Memory Cleanup ---
    // Remove from GList
    if (current_section) {
        current_section->entries = g_list_remove(current_section->entries, entry);
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(current_section->list));
    }

    // Destroy the GtkListBoxRow. This will automatically trigger pdf_entry_destroy_notify
    // because the PdfEntry struct is attached to the row with g_object_set_data_full.
    if (row) {
        gtk_widget_destroy(GTK_WIDGET(row));
        g_print("PDF entry removed from GUI. PdfEntry struct will be freed by GDestroyNotify.\n");
    } else {
        g_warning("GUI context (row) invalid for full GUI removal. PdfEntry struct will not be automatically freed by GTK.");
        // This case should ideally not happen if add_pdf_entry_to_gui is always used.
        // If it does, the PdfEntry would be leaked.
        // However, with the new model, the PdfEntry is *always* attached to a row.
        // So, this branch is effectively unreachable if the row was properly created.
    }
}


/**
 * @brief Callback function for when a professor is selected from the dropdown (`professor_combo`).
 * This function clears all existing PDF entries from the GUI and then initiates
 * a scan of all relevant category folders for the newly selected professor,
 * populating the GUI with found PDFs.
 *
 * @param combo_box The GtkComboBox that emitted the "changed" signal.
 * @param user_data User data (not used in this function).
 * @return G_SOURCE_REMOVE to remove the idle source if called via g_idle_add.
 */
gboolean on_professor_selected(GtkComboBox *combo_box, gpointer user_data) {
    (void)user_data; // Suppress unused parameter warning

    const gchar *professor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_box));

    // Section: Initial GUI State Update
    // Hide the preview report button immediately when a new professor is selected or cleared.
    if (preview_report_btn) {
        gtk_widget_hide(preview_report_btn);
    }

    // Set status label and progress bar to indicate loading is in progress.
    gtk_label_set_text(GTK_LABEL(status_label), "Carregando informações do professor...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gchar *progress_text_0 = g_strdup_printf("%d%%", 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0);
    SAFE_FREE(progress_text_0); // Free the string after use.
    g_main_context_iteration(NULL, FALSE); // Allow GUI to update immediately.

    // Section: Clear Existing Entries
    // Clear all existing PDF entries from all categories before scanning new ones.
    // This prevents accumulation of PDFs from previous selections or manual additions.
    for (gint i = 0; i < global_num_categories; i++) {
        // Destroy all GtkListBoxRows in the current category's listbox.
        // This will trigger `pdf_entry_destroy_notify` for each associated `PdfEntry`.
        GList *children = gtk_container_get_children(GTK_CONTAINER(global_sections[i].list));
        for (GList *l = children; l != NULL; l = g_list_next(l)) {
            gtk_widget_destroy(GTK_WIDGET(l->data));
        }
        g_list_free(children); // Free the GList structure.

        // Clear the `GList` of `PdfEntry` pointers for this section, as the entries are now destroyed.
        if (global_sections[i].entries) {
            g_list_free(global_sections[i].entries);
            global_sections[i].entries = NULL;
        }
    }

    // Free previous `global_max_category_counters` array if it exists and reallocate/initialize to zero.
    SAFE_FREE(global_max_category_counters);
    global_max_category_counters = g_new0(gint, global_num_categories); // Allocate and initialize all to 0.


    if (!professor) {
        g_print("No professor selected or selection cleared. All entries cleared.\n");
        // Reset status and progress bar to idle state.
        gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_0_again = g_strdup_printf("%d%%", 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0_again);
        SAFE_FREE(progress_text_0_again); // Free the string after use.
        g_main_context_iteration(NULL, FALSE);
        return G_SOURCE_REMOVE; // Return G_SOURCE_REMOVE if called via g_idle_add.
    }

    // NEW: Check if exiftool is available
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
        SAFE_FREE(progress_text_error); // Free the string after use.
        g_main_context_iteration(NULL, FALSE);
        SAFE_FREE(exiftool_path);
        return G_SOURCE_REMOVE; // Return G_SOURCE_REMOVE if called via g_idle_add.
    }
    SAFE_FREE(exiftool_path); // Free the path once checked

    g_print("Professor selected: %s. Initiating folder scan for all categories.\n", professor);

    // Section: Scan Category Folders and Populate GUI
    // Iterate through each category and process its corresponding folder for the selected professor.
    for (gint i = 0; i < global_num_categories; i++) {
        // FIX: Changed professor_base_dir to "./" as per user's specified structure
        gchar *professor_base_dir = g_strdup("./"); // Base directory for professor folders.
        // Construct the full path to the category folder for the current professor.
        gchar *category_folder_path = g_strdup_printf("%s%s/%s", professor_base_dir, professor, global_categories[i]);

        // Call `process_pdf_folder` to scan the folder and add PDFs to the GUI.
        // This function now spawns asynchronous processes for exiftool operations.
        process_pdf_folder(category_folder_path, i);

        // Set the sort function for this category's listbox.
        gtk_list_box_set_sort_func(GTK_LIST_BOX(global_sections[i].list), (GtkListBoxSortFunc)compare_pdf_entries_by_counter, NULL, NULL);
        gtk_list_box_invalidate_sort(GTK_LIST_BOX(global_sections[i].list)); // Apply initial sort.

        SAFE_FREE(category_folder_path); // Free the allocated path string.
        SAFE_FREE(professor_base_dir);   // Free the base directory string.
    }

    // The final status update for the loading phase will be handled by
    // `exiftool_read_completed_callback` once all asynchronous exiftool reads are done.
    // We will set a final status here, assuming the processes will eventually update.
    gtk_label_set_text(GTK_LABEL(status_label), "Carregamento iniciado. Aguardando metadados...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.05); // Small initial progress
    gchar *progress_text_5 = g_strdup_printf("%d%%", 5);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_5);
    SAFE_FREE(progress_text_5); // Free the string after use.
    g_main_context_iteration(NULL, FALSE); // Allow GUI to update

    return G_SOURCE_REMOVE; // Return G_SOURCE_REMOVE if called via g_idle_add.
}

/**
 * @brief Callback for the "Abrir Pasta" (Open Folder) button in a category section.
 * This function attempts to open the corresponding category folder for the currently
 * selected professor using the system's default file manager.
 *
 * @param button The GtkButton that emitted the "clicked" signal.
 * @param user_data A gpointer containing the category index (GINT_TO_POINTER).
 */
void on_open_category_folder(GtkWidget *button, gint category_index) {
    (void)button; // Suppress unused parameter warning

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

    gchar *professor_base_dir = g_strdup("./"); // Base directory for professor folders.
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
 * @brief Processes a given folder, identifies PDF files, and initiates asynchronous
 * metadata extraction for each PDF. It then adds the PDF entries to the GUI
 * for a specific category.
 *
 * @param folder_path The full path to the folder to scan for PDF files.
 * @param category_index The index of the category (`global_sections`) to which
 * the found PDF entries belong.
 */
void process_pdf_folder(const gchar *folder_path, gint category_index) {
    DIR *d;             // Directory stream pointer.
    struct dirent *dir; // Directory entry structure.
    g_print("Starting PDF folder scan in: %s for category index %d\n", folder_path, category_index);

    if (!is_valid_path(folder_path)) {
        g_warning("Invalid folder path provided to process_pdf_folder: %s", folder_path);
        return;
    }

    // Section: Open Directory and Iterate Files
    d = opendir(folder_path); // Open the directory.
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            // Skip current directory (".") and parent directory ("..") entries.
            if (g_strcmp0(dir->d_name, ".") == 0 || g_strcmp0(dir->d_name, "..") == 0) {
                continue;
            }

            // Construct the full path for each entry in the directory.
            gchar *full_path = g_build_filename(folder_path, dir->d_name, NULL);
            if (!is_valid_path(full_path)) {
                g_warning("Skipping invalid file path: %s", full_path);
                SAFE_FREE(full_path);
                continue;
            }

            // Use GFile to query file information (more robust than `stat()`).
            GFile *file = g_file_new_for_path(full_path);
            GError *file_info_error = NULL;
            GFileInfo *file_info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL, &file_info_error);

            // Section: Process PDF Files
            if (file_info && g_file_info_get_file_type(file_info) == G_FILE_TYPE_REGULAR) {
                gchar *lower_case_filename = g_ascii_strdown(dir->d_name, -1); // Convert filename to lowercase for case-insensitive check.
                // Check if it's a regular file and ends with ".pdf".
                if (g_str_has_suffix(lower_case_filename, ".pdf")) {
                    g_print("  Found PDF: %s\n", full_path);

                    // Create a new `PdfEntry` struct to hold the PDF's data and GUI elements.
                    PdfEntry *new_entry = create_pdf_entry();
                    new_entry->file_path = g_strdup(full_path); // Store the full path.

                    GtkListBoxRow *row = create_pdf_entry_row(new_entry, category_index);
                    // Set the file chooser button's filename (this will trigger `on_file_set` for this entry).
                    // This is done to ensure the file_chooser_button reflects the loaded file.
                    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(new_entry->file_btn), full_path);


                    // Add the entry to the GUI immediately with placeholder text.
                    // The actual metadata will be populated later by `exiftool_read_completed_callback`.
                    add_pdf_entry_to_gui(category_index, new_entry, row);
                    gtk_entry_set_text(GTK_ENTRY(new_entry->title_entry), "Carregando título...");
                    gtk_entry_set_text(GTK_ENTRY(new_entry->year_entry), "Carregando ano...");
                    gtk_entry_set_text(GTK_ENTRY(new_entry->category_entry), "Carregando categoria...");

                    // Allocate and populate the `ExiftoolReadOperation` struct for the asynchronous task.
                    ExiftoolReadOperation *op = g_malloc0(sizeof(ExiftoolReadOperation));
                    op->file_path = g_strdup(full_path); // Duplicate path for the process.
                    op->gui_row = row;           // Pointer to the GUI row to update.
                    op->category_index = category_index; // Category index for updating max counters.
                    op->is_new_file_selection = FALSE;   // Flag indicating this is part of a folder scan.

                    // Spawn exiftool asynchronously
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
                SAFE_FREE(lower_case_filename); // Free the lowercase filename string.
            }
            // Unreference GFile and GFileInfo objects to release resources.
            if (file_info) g_object_unref(file_info);
            if (file_info_error) g_error_free(file_info_error); // Free error if any
            if (file) g_object_unref(file);
            SAFE_FREE(full_path); // Free the full path string.
        }
        closedir(d); // Close the directory stream.
    } else {
        // Warning if the directory cannot be opened.
        g_warning("Could not open category directory: %s (%s). Skipping this category.", folder_path, g_strerror(errno));
    }
}

/**
 * @brief Helper function to save a single PDF entry. This involves:
 * 1. Validating input fields (file path, title, year, category).
 * 2. Sanitizing the category for use in the new filename.
 * 3. Determining the new filename, incorporating a counter (manual, detected, or new).
 * 4. Ensuring the destination category directory exists.
 * 5. Copying the original PDF file to its new location with the new filename.
 * 6. Injecting the title, year, and category metadata into the *copied* PDF using `exiftool`.
 * 7. Optionally moving the *original* file to an 'old_files' subfolder if its filename changed.
 *
 * @param entry The `PdfEntry` struct containing the data to be saved.
 * @param professor_name The name of the currently selected professor.
 * @param category_index The index of the category this entry belongs to.
 * @return TRUE on successful saving and processing of the entry, FALSE on failure.
 */
gboolean save_single_pdf_entry(PdfEntry *entry, const gchar *professor_name, gint category_index) {
    // Section: Retrieve Data from GUI
    const gchar *title_raw = gtk_entry_get_text(GTK_ENTRY(entry->title_entry));
    const gchar *year = gtk_entry_get_text(GTK_ENTRY(entry->year_entry));
    const gchar *manual_counter_str = gtk_entry_get_text(GTK_ENTRY(entry->counter_entry));
    const gchar *current_category_text = gtk_entry_get_text(GTK_ENTRY(entry->category_entry));

    // Section: Basic Input Validation
    // Ensure a file is selected and title/year/category fields are not empty.
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

    // Section: Sanitize Category for Filename
    gchar *sanitized_category_for_filename = sanitize_filename(current_category_text);

    // Section: Determine Final Filename with Counter
    gchar *final_filename_no_ext = NULL;
    gint current_counter = 0;
    gboolean use_manual_counter_for_filename = FALSE;

    // Check if a valid manual counter was provided.
    if (manual_counter_str && g_strcmp0(manual_counter_str, "") != 0) {
        gint parsed_counter = atoi(manual_counter_str);
        if (parsed_counter > 0) {
            current_counter = parsed_counter;
            use_manual_counter_for_filename = TRUE;
            g_print("    Using manual counter: %02d\n", current_counter);
        }
    }

    if (use_manual_counter_for_filename) {
        // If manual counter is used, format filename with it.
        // NEW FILENAME FORMAT: NN_category_YEAR.pdf
        final_filename_no_ext = g_strdup_printf("%02d_%s_%s", current_counter, sanitized_category_for_filename, year);
    } else {
        // If no valid manual counter, use the original detected counter if it exists.
        if (entry->has_original_counter && entry->original_detected_counter > 0) {
            current_counter = entry->original_detected_counter;
            g_print("    Using detected original counter: %02d\n", current_counter);
        } else {
            // If no original counter, assign a new, incremented counter for this category.
            // No mutex needed here as global_max_category_counters is only accessed on main thread
            global_max_category_counters[category_index]++; // Increment the max counter for this category.
            current_counter = global_max_category_counters[category_index];
            g_print("    Assigning new counter: %02d\n", current_counter);
        }
        // NEW FILENAME FORMAT: NN_category_YEAR.pdf
        final_filename_no_ext = g_strdup_printf("%02d_%s_%s", current_counter, sanitized_category_for_filename, year);
    }

    // Section: Ensure Destination Directory Exists
    // FIX: Changed professor_base_dir to "./" as per user's specified structure
    gchar *professor_base_dir = g_strdup("./"); // Base directory for professor folders.
    // Construct the destination directory path for the current PDF (e.g., "./Professor A/Category Name").
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

    // Create the category directory and its parents if they don't exist.
    if (!g_mkdir_with_parents(dest_category_dir, 0755)) {
        if (errno != EEXIST) { // If it's not "already exists", it's a real error.
            g_warning("Failed to create category directory: %s (%s)", dest_category_dir, g_strerror(errno));
            // Free allocated resources on error.
            SAFE_FREE(sanitized_category_for_filename);
            SAFE_FREE(final_filename_no_ext);
            SAFE_FREE(dest_category_dir);
            SAFE_FREE(professor_base_dir);
            return FALSE;
        }
    }
    g_print("    Category directory exists (or was created): %s\n", dest_category_dir);

    // Section: Construct Destination File Path
    gchar *dest_file_name = g_strdup_printf("%s.pdf", final_filename_no_ext); // Add .pdf extension.
    gchar *dest_file_path = g_strdup_printf("%s/%s", dest_category_dir, dest_file_name); // Full destination path.
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

    // Section: Copy File
    // Create GFile objects for source and destination (required by `g_file_copy`).
    GFile *source_file = g_file_new_for_path(entry->file_path);
    GFile *destination_file = g_file_new_for_path(dest_file_path);

    GError *copy_error = NULL; // Renamed to avoid conflict with 'error' in exiftool section
    // Copy the file, overwriting if a file with the same name already exists at the destination.
    if (!g_file_copy(source_file, destination_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &copy_error)) {
        g_warning("Failed to copy file from %s to %s: %s", entry->file_path, dest_file_path, copy_error ? copy_error->message : "N/A");
        if (copy_error) g_error_free(copy_error);
        // Free allocated resources on error.
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

    // Unreference GFile objects.
    if (source_file) g_object_unref(source_file);
    if (destination_file) g_object_unref(destination_file);

    // Section: Inject Metadata with Exiftool using g_spawn_sync
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
        // Free allocated resources on error.
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
        NULL,       // working_directory
        exiftool_argv, // argv
        NULL,       // envp
        G_SPAWN_SEARCH_PATH, // flags
        NULL,       // child_setup
        NULL,       // user_data
        &exiftool_stdout, // stdout_buf
        &exiftool_stderr, // stderr_buf
        &exiftool_exit_status, // exit_status
        &exiftool_error // error
    );

    if (exiftool_argv) g_strfreev(exiftool_argv);
    SAFE_FREE(exiftool_command_line);
    SAFE_FREE(exiftool_stdout); // We don't need the stdout for writing, just stderr for errors
    SAFE_FREE(exiftool_stderr);

    if (!exiftool_success || exiftool_exit_status != 0) {
        g_warning("Failed to update metadata for %s. Exiftool command failed with exit code: %d. Error: %s",
                  dest_file_path, exiftool_exit_status, exiftool_error ? exiftool_error->message : "N/A");
        if (exiftool_error) g_error_free(exiftool_error);
    } else {
        g_print("    Metadata successfully written to %s.\n", dest_file_path);
    }

    // Section: Conditional Move Original File to old_files directory
    // Determine if the filename has changed compared to its original name.
    gboolean filename_changed = (entry->original_filename_no_ext == NULL || g_strcmp0(final_filename_no_ext, entry->original_filename_no_ext) != 0);

    // If the filename changed AND the original file path is still valid, move the original.
    if (filename_changed && entry->file_path != NULL && g_file_test(entry->file_path, G_FILE_TEST_EXISTS)) {
        gchar *old_files_dir = g_strdup_printf("%s/old_files", dest_category_dir); // Path to 'old_files' subfolder.
        g_print("    Attempting to ensure old_files directory exists: %s\n", old_files_dir);

        if (!is_valid_path(old_files_dir)) {
            g_warning("Invalid old_files directory path: %s", old_files_dir);
            SAFE_FREE(old_files_dir);
            // Continue with cleanup and return TRUE, as the main operation (copy and metadata) succeeded.
        } else {
            // Create 'old_files' directory if it doesn't exist.
            if (!g_mkdir_with_parents(old_files_dir, 0755)) {
                if (errno != EEXIST) {
                    g_warning("Failed to create old_files directory: %s (%s)", old_files_dir, g_strerror(errno));
                }
            } else {
                g_print("    Old_files directory exists (or was created): %s\n", old_files_dir);
            }

            gchar *original_basename = g_path_get_basename(entry->file_path); // Get original filename.
            gchar *old_file_dest_path = g_strdup_printf("%s/%s", old_files_dir, original_basename); // Destination for original.
            g_print("    Moving original file from %s to %s\n", entry->file_path, old_file_dest_path);

            if (!is_valid_path(old_file_dest_path)) {
                g_warning("Invalid old file destination path: %s", old_file_dest_path);
                SAFE_FREE(original_basename);
                SAFE_FREE(old_file_dest_path);
                SAFE_FREE(old_files_dir);
                // Continue with cleanup and return TRUE.
            } else {
                GError *move_error = NULL;
                GFile *original_gfile_to_move = g_file_new_for_path(entry->file_path);
                GFile *old_dest_gfile = g_file_new_for_path(old_file_dest_path);

                // Check if the original file still exists before attempting to move it.
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

                // Unreference GFile objects and free allocated strings.
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

    // Section: Final Cleanup
    // Free all other dynamically allocated strings.
    SAFE_FREE(sanitized_category_for_filename);
    SAFE_FREE(final_filename_no_ext);
    SAFE_FREE(dest_file_name);
    SAFE_FREE(dest_file_path);
    SAFE_FREE(dest_category_dir);
    SAFE_FREE(professor_base_dir);

    return TRUE;
}

/**
 * @brief Callback for when the asynchronous report generation process finishes.
 * This function is called by `g_child_watch_add` when the backend script
 * (`generate_pdf_professor_v2.0.1-R22.sh`) completes its execution.
 * It displays a success or error message, updates the GUI status, and attempts
 * to open the generated PDF.
 *
 * @param pid The process ID of the finished child process.
 * @param status The exit status of the child process.
 * @param user_data User data (in this case, the professor's name, which needs to be freed).
 */
void on_report_generation_finished(GPid pid, gint status, gpointer user_data) {
    gchar *professor_name = (gchar *)user_data; // Retrieve the professor name.
    g_print("Report generation process (PID: %d) finished with status: %d\n", pid, status);

    g_spawn_close_pid(pid); // Clean up the child process resources.

    // Section: Check Exit Status
    // `WIFEXITED` checks if the child process exited normally.
    // `WEXITSTATUS` gets the exit status if it exited normally (0 for success).
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        // Section: Success Case
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_INFO,
                                                   GTK_BUTTONS_OK,
                                                   "Relatório gerado com sucesso!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        gchar *sanitized_professor_name = sanitize_filename(professor_name); // Sanitize name for filename.
        // Construct the expected path of the professional report PDF.
        gchar *pdf_filename = g_strdup_printf("final/%s_relatorio_profissional.pdf", sanitized_professor_name);
        GError *spawn_error = NULL;

        // Attempt to open the generated PDF using `xdg-open`.
        if (g_file_test(pdf_filename, G_FILE_TEST_EXISTS)) {
            gchar *xdg_open_command = g_strdup_printf("xdg-open \"%s\"", pdf_filename);
            g_print("Attempting to open PDF with command: %s\n", xdg_open_command);

            if (!g_spawn_command_line_async(xdg_open_command, &spawn_error)) {
                g_warning("Failed to launch PDF viewer via xdg-open: %s", spawn_error ? spawn_error->message : "N/A");
                if (spawn_error) g_error_free(spawn_error);
            } else {
                g_print("PDF viewer launched successfully (via xdg-open).\n");
            }
            SAFE_FREE(xdg_open_command); // Free the command string.
            // Show the preview button only if the PDF exists after successful generation.
            if (preview_report_btn) {
                gtk_widget_show(preview_report_btn);
            }
        } else {
            g_warning("Generated PDF not found at path: %s", pdf_filename);
            // Keep the button hidden if the PDF is not found.
            if (preview_report_btn) {
                gtk_widget_hide(preview_report_btn);
            }
        }
        SAFE_FREE(pdf_filename);          // Free allocated PDF filename.
        SAFE_FREE(sanitized_professor_name); // Free allocated sanitized name.

        // Update GUI status and progress bar to 100%.
        gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 1.0);
        gchar *progress_text_100 = g_strdup_printf("%d%%", 100);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_100);
        SAFE_FREE(progress_text_100); // Free the string after use.

    } else {
        // Section: Error Case
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Falha ao gerar o relatório. Verifique o log do script no terminal que foi aberto.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_warning("Backend script failed with exit status: %d", status);
        // Update GUI status and progress bar to indicate an.
        gtk_label_set_text(GTK_LABEL(status_label), "Erro na geração do relatório.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error); // Free the string after use.
        // Keep the button hidden if generation failed.
        if (preview_report_btn) {
            gtk_widget_hide(preview_report_btn);
        }
    }
    SAFE_FREE(professor_name); // Free the professor name passed as user_data.
}


/**
 * @brief Callback for the "Gerar Relatório Profissional" button.
 * This function orchestrates the entire report generation process:
 * 1. Validates that a professor is selected.
 * 2. Hides the preview report button.
 * 3. Updates GUI status and progress bar.
 * 4. Ensures the professor's main directory exists.
 * 5. Iterates through all PDF entries for the selected professor and saves their data.
 * 6. Spawns the backend script (`generate_pdf_professor_v2.0.1-R22.sh`) in a new
 * terminal window to compile the LaTeX report asynchronously.
 *
 * @param widget The GtkButton that emitted the "clicked" signal.
 * @param data User data (not used in this function).
 */
void generate_report(GtkWidget *widget, gpointer data) {
    (void)widget; // Suppress unused parameter warning
    (void)data;   // Suppress unused parameter warning

    // Section: Input Validation - Professor Selection
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

    // Section: Initial GUI State Update for Generation
    // Hide the preview report button immediately when generation starts.
    if (preview_report_btn) {
        gtk_widget_hide(preview_report_btn);
    }

    // Set status label and progress bar to indicate the start of report generation.
    gtk_label_set_text(GTK_LABEL(status_label), "Iniciando geração do relatório...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.2); // Start from 20% (after loading phase).
    gchar *progress_text_20 = g_strdup_printf("%d%%", 20);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_20);
    SAFE_FREE(progress_text_20); // Free the string after use.
    g_main_context_iteration(NULL, FALSE); // Allow GUI to update.

    // Section: Ensure Professor Directory Exists
    // FIX: Changed professor_base_dir to "./" as per user's specified structure
    gchar *professor_base_dir = g_strdup("./"); // Base directory for professor folders.
    g_print("  Base directory for professor folders: %s\n", professor_base_dir);

    // Construct the professor's main folder path (e.g., "./Professor A").
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
        // Update GUI status to reflect the error.
        gtk_label_set_text(GTK_LABEL(status_label), "Erro ao criar diretório do professor.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error); // Free the string after use.
        g_main_context_iteration(NULL, FALSE);
        return;
    }

    // Ensure the professor's main directory exists. This is non-destructive.
    if (!g_mkdir_with_parents(professor_folder_path, 0755)) {
        if (errno != EEXIST) { // If it's not "already exists", it's a real error.
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
            // Update GUI status to reflect the error.
            gtk_label_set_text(GTK_LABEL(status_label), "Erro ao criar diretório do professor.");
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
            gchar *progress_text_error = g_strdup_printf("Erro!");
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
            SAFE_FREE(progress_text_error); // Free the string after use.
            g_main_context_iteration(NULL, FALSE);
            return;
        }
    }
    g_print("  Professor directory exists (or was created): %s\n", professor_folder_path);
    SAFE_FREE(professor_folder_path); // Free the allocated path.

    // Section: Calculate Total PDFs to Process
    gint total_pdfs_to_process = 0;
    for (gint i = 0; i < global_num_categories; i++) {
        total_pdfs_to_process += g_list_length(global_sections[i].entries);
    }
    g_print("Total PDFs to process: %d\n", total_pdfs_to_process);

    gint processed_pdfs_count = 0;

    // Section: Iterate and Save Individual PDF Entries
    // This loop processes each PDF entry, copies it to the correct location,
    // and injects metadata. Progress bar is updated during this phase.
    for (gint i = 0; i < global_num_categories; i++) {
        g_print("Processing category: %s\n", global_categories[i]);
        GList *l;
        for (l = global_sections[i].entries; l != NULL; l = g_list_next(l)) {
            PdfEntry *entry = (PdfEntry *)l->data;
            if (save_single_pdf_entry(entry, professor, i)) {
                processed_pdfs_count++;
                // Calculate progress fraction (from 20% to 90%).
                gdouble fraction_of_processing = (gdouble)processed_pdfs_count / total_pdfs_to_process;
                gdouble overall_fraction = 0.2 + (fraction_of_processing * 0.7); // 70% of total progress for saving.
                gchar *progress_text = g_strdup_printf("Processando PDF %d de %d: %s",
                                                       processed_pdfs_count, total_pdfs_to_process, gtk_entry_get_text(GTK_ENTRY(entry->title_entry)));
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), overall_fraction);
                gchar *progress_percent_text = g_strdup_printf("%d%%", (gint)(overall_fraction * 100));
                gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_percent_text);
                SAFE_FREE(progress_percent_text); // Free the string after use.
                gtk_label_set_text(GTK_LABEL(status_label), progress_text);
                SAFE_FREE(progress_text);
                g_main_context_iteration(NULL, FALSE); // Allow GUI to update.
            } else {
                g_warning("Failed to save PDF entry: %s", entry->file_path ? entry->file_path : "N/A");
            }
        }
    }

    SAFE_FREE(professor_base_dir); // Free allocated base directory string.

    // Section: Find Terminal Path
    // Find the full path to `gnome-terminal` (or another suitable terminal).
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
        // Update GUI status for terminal not found error.
        gtk_label_set_text(GTK_LABEL(status_label), "Erro: Terminal não encontrado.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error);
        SAFE_FREE(progress_text_error); // Free the string after use.
        if (preview_report_btn) {
            gtk_widget_hide(preview_report_btn);
        }
        return;
    }


    gchar *script_path = g_strdup("./generate_pdf_professor_v2.0.1-R22.sh"); // Path to the backend script.
    gchar *professor_arg_quoted = g_shell_quote(professor); // Quote professor name for shell safety.

    // Construct the command to run the script inside `gnome-terminal`.
    // `bash -c '...'` executes the script and then waits for user input before closing the terminal.
    gchar *command_in_terminal = g_strdup_printf("%s %s; echo \"\"; echo \"Pressione Enter para fechar esta janela...\"; read -n 1",
                                                  script_path, professor_arg_quoted);

    // Prepare arguments for `g_spawn_async`.
    gchar *argv_terminal[] = {
        terminal_path,           // Full path to the terminal executable.
        g_strdup("--wait"),      // Make gnome-terminal wait for the command to finish.
        g_strdup("--"),          // Separator for gnome-terminal arguments vs. command to execute.
        g_strdup("bash"),        // Execute bash.
        g_strdup("-c"),          // Tell bash to execute a command string.
        command_in_terminal,     // The actual command string to execute in bash.
        NULL                     // Null-terminate the argument list.
    };

    GPid child_pid;      // Process ID of the spawned child.
    GError *spawn_error = NULL;

    // Section: Update GUI Status for Script Execution
    gtk_label_set_text(GTK_LABEL(status_label), "Abrindo terminal para compilar relatório LaTeX...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.9); // Set progress to 90%.
    gchar *progress_text_90 = g_strdup_printf("%d%%", 90);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_90);
    SAFE_FREE(progress_text_90); // Free the string after use.
    g_main_context_iteration(NULL, FALSE); // Update GUI before spawning.

    // Section: Spawn Backend Script Asynchronously
    // `g_spawn_async` runs the command in a new process.
    // `G_SPAWN_DO_NOT_REAP_CHILD` is important so `g_child_watch_add` can monitor the child.
    if (!g_spawn_async(NULL,                    // Working directory (NULL for current).
                       argv_terminal,           // Argument vector.
                       NULL,                    // Environment variables (NULL for current).
                       G_SPAWN_DO_NOT_REAP_CHILD, // Flags.
                       NULL,                    // Child setup function.
                       NULL,                    // User data for child setup.
                       &child_pid,              // Output: child process ID.
                       &spawn_error)) {
        // Error handling if spawning fails.
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Falha ao iniciar o terminal para geração de relatório: %s", spawn_error ? spawn_error->message : "N/A");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (spawn_error) g_error_free(spawn_error);
        // Update GUI status for spawn error.
        gtk_label_set_text(GTK_LABEL(status_label), "Erro ao iniciar geração.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_error_again = g_strdup_printf("Erro!");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_error_again);
        SAFE_FREE(progress_text_error_again); // Free the string after use.
        if (preview_report_btn) {
            gtk_widget_hide(preview_report_btn);
        }
    } else {
        g_print("Terminal spawned successfully with PID: %d\n", child_pid);
        // Add a child watch to be notified when the terminal process exits.
        // `g_strdup(professor)` duplicates the professor name to be freed in the callback.
        g_child_watch_add(child_pid, on_report_generation_finished, g_strdup(professor));
    }

    // Section: Cleanup Allocated Argument Strings
    // Free all dynamically allocated strings used for `argv_terminal`.
    SAFE_FREE(terminal_path);
    SAFE_FREE(argv_terminal[1]); // "--wait"
    SAFE_FREE(argv_terminal[2]); // "--"
    SAFE_FREE(argv_terminal[3]); // "bash"
    SAFE_FREE(argv_terminal[4]); // "-c"
    SAFE_FREE(argv_terminal[5]); // The formatted `command_in_terminal` string.
    SAFE_FREE(script_path);
    SAFE_FREE(professor_arg_quoted);
}

/**
 * @brief Callback for the "Salvar Alterações" button.
 * This function iterates through all `PdfEntry` items across all categories
 * for the currently selected professor and saves their current state (counter, title, year, category)
 * by copying and updating the corresponding PDF files. It does *not* generate the full report.
 *
 * IMPORTANT: This function does NOT trigger a full folder rescan or reload of the GUI.
 * It only updates the individual PDF files and provides progress feedback.
 *
 * @param button The GtkButton that emitted the "clicked" signal.
 * @param user_data User data (not used in this function).
 */
void on_save_all_entries(GtkWidget *button, gpointer user_data) {
    (void)button;    // Suppress unused parameter warning
    (void)user_data; // Suppress unused parameter warning

    // Section: Input Validation - Professor Selection
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

    // Section: Initial GUI Status Update
    gtk_label_set_text(GTK_LABEL(status_label), "Salvando alterações...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gchar *progress_text_0 = g_strdup_printf("%d%%", 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0);
    SAFE_FREE(progress_text_0); // Free the string after use.
    g_main_context_iteration(NULL, FALSE); // Allow GUI to update.

    // Section: Calculate Total PDFs to Save
    gint total_pdfs_to_save = 0;
    for (gint i = 0; i < global_num_categories; i++) {
        total_pdfs_to_save += g_list_length(global_sections[i].entries);
    }

    // Section: Handle No PDFs to Save
    if (total_pdfs_to_save == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_INFO,
                                                   GTK_BUTTONS_OK,
                                                   "Não há PDFs para salvar.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        // Reset status and progress bar to idle.
        gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
        gchar *progress_text_0_again = g_strdup_printf("%d%%", 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_0_again);
        SAFE_FREE(progress_text_0_again); // Free the string after use.
        g_main_context_iteration(NULL, FALSE);
        return;
    }

    // NEW: Check if exiftool is available before attempting to save
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
        SAFE_FREE(progress_text_error); // Free the string after use.
        g_main_context_iteration(NULL, FALSE);
        SAFE_FREE(exiftool_path);
        return;
    }
    SAFE_FREE(exiftool_path); // Free the path once checked


    gint saved_pdfs_count = 0;
    // Section: Iterate and Save Individual PDF Entries
    // Loop through each category and then through each PdfEntry within that category.
    for (gint i = 0; i < global_num_categories; i++) {
        GList *l;
        for (l = global_sections[i].entries; l != NULL; l = g_list_next(l)) {
            PdfEntry *entry = (PdfEntry *)l->data;
            if (save_single_pdf_entry(entry, professor, i)) {
                saved_pdfs_count++;
            } else {
                g_warning("Failed to save PDF entry during 'Salvar Alterações': %s", entry->file_path ? entry->file_path : "N/A");
            }

            // Update progress bar and status label.
            gdouble fraction = (gdouble)saved_pdfs_count / total_pdfs_to_save;
            gchar *progress_text = g_strdup_printf("Salvando PDF %d de %d: %s",
                                                   saved_pdfs_count, total_pdfs_to_save, gtk_entry_get_text(GTK_ENTRY(entry->title_entry)));
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), fraction);
            gchar *progress_percent_text = g_strdup_printf("%d%%", (gint)(fraction * 100));
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_percent_text);
            SAFE_FREE(progress_percent_text); // Free the string after use.
            gtk_label_set_text(GTK_LABEL(status_label), progress_text);
            SAFE_FREE(progress_text);
            g_main_context_iteration(NULL, FALSE); // Allow GUI to update.
        }
    }

    // Section: Final Success Message and GUI Update
    GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK,
                                               "Alterações salvas com sucesso!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    // After saving, check if the professional report PDF exists and update button visibility.
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

    // Set final status and progress bar to 100%.
    gtk_label_set_text(GTK_LABEL(status_label), "Pronto.");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 1.0);
    gchar *progress_text_100 = g_strdup_printf("%d%%", 100);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), progress_text_100);
    SAFE_FREE(progress_text_100); // Free the string after use.
    g_main_context_iteration(NULL, FALSE); // Allow GUI to update.

    // NEW: Auto-reload the professor after saving changes, deferring it to avoid race conditions
    g_print("Scheduling auto-reload of professor after saving changes...\n");
    // Use g_idle_add to ensure the reload happens after the current event loop iteration completes,
    // allowing GTK to finish any pending operations on the current GUI state.
    // The professor_combo widget itself is passed as user_data, which will be received by
    // the on_professor_selected callback as its first argument (GtkComboBox *combo_box).
    g_idle_add(on_professor_selected_idle_wrapper, professor_combo); // Use the wrapper function
}

/**
 * @brief Wrapper function to call on_professor_selected from g_idle_add.
 * This function matches the GSourceFunc signature (gboolean (*func) (gpointer user_data))
 * and safely calls on_professor_selected with the correct argument type.
 * @param user_data A gpointer to the GtkComboBox (professor_combo).
 * @return G_SOURCE_REMOVE to remove the idle source after execution.
 */
gboolean on_professor_selected_idle_wrapper(gpointer user_data) {
    GtkComboBox *combo_box = GTK_COMBO_BOX(user_data);
    return on_professor_selected(combo_box, NULL); // Pass NULL for the original user_data in on_professor_selected
}


/**
 * @brief Callback for the "Criar Pasta do Professor" button.
 * This function creates the main directory for the currently selected professor
 * and all its subdirectories for each defined category if they do not already exist.
 *
 * @param button The GtkButton that emitted the "clicked" signal.
 * @param user_data User data (not used in this function).
 */
void on_create_professor_folder_clicked(GtkWidget *button, gpointer user_data) {
    (void)button; // Suppress unused parameter warning
    (void)user_data; // Suppress unused parameter warning

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

    gchar *professor_base_dir = g_strdup("./"); // Base directory for professor folders.
    gchar *professor_folder_path = g_strdup_printf("%s%s", professor_base_dir, professor);
    gboolean success = TRUE;
    gchar *message = NULL;

    // Create professor's main directory
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
        // Create category subdirectories
        for (gint i = 0; i < global_num_categories; i++) {
            gchar *category_folder_path = g_strdup_printf("%s/%s", professor_folder_path, global_categories[i]);
            if (!g_mkdir_with_parents(category_folder_path, 0755)) {
                if (errno != EEXIST) {
                    message = g_strdup_printf("Falha ao criar o diretório da categoria '%s' para '%s': %s",
                                              global_categories[i], professor, g_strerror(errno));
                    success = FALSE;
                    SAFE_FREE(category_folder_path);
                    break; // Exit loop on first error
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

    // After creating folders, it might be good to reload the professor's view
    // to reflect any changes if new empty folders were created.
    on_professor_selected(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Callback for the "Visualizar" (Preview) button on a PdfEntry.
 * This function attempts to open the PDF file associated with the PdfEntry
 * using the system's default application (e.g., a PDF viewer).
 *
 * @param button The GtkButton that emitted the "clicked" signal.
 * @param entry The PdfEntry struct associated with the button.
 */
void on_preview_pdf(GtkWidget *button, PdfEntry *entry) {
    (void)button; // Suppress unused parameter warning

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
 * @brief Callback for the "Visualizar Relatório Profissional" (Preview Professional Report) button.
 * This function attempts to open the generated professional report PDF for the currently
 * selected professor using the system's default PDF viewer.
 *
 * @param button The GtkButton that emitted the "clicked" signal.
 * @param user_data User data (not used in this function).
 */
void on_preview_report_pdf(GtkWidget *button, gpointer user_data) {
    (void)button;    // Suppress unused parameter warning
    (void)user_data; // Suppress unused parameter warning

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
        // Corrected arguments for gtk_message_dialog_new
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR, // GtkMessageType
                                                   GTK_BUTTONS_OK,    // GtkButtonsType
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
 * @brief Callback for the "Recarregar Professor" button.
 * This function simply calls `on_professor_selected` with the currently
 * selected professor, effectively reloading all PDF entries and their data
 * for the current professor from disk.
 *
 * @param button The GtkButton that emitted the "clicked" signal.
 * @param user_data User data (not used in this function).
 */
void on_reload_professor_button_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;    // Suppress unused parameter warning
    (void)user_data; // Suppress unused parameter warning

    g_print("Reload professor button clicked. Triggering on_professor_selected.\n");
    // Directly call on_professor_selected to re-trigger the loading logic.
    // We pass professor_combo as the first argument, and NULL for user_data.
    // The return value of on_professor_selected is G_SOURCE_REMOVE, but we don't
    // need to capture it here as we are not using g_idle_add in this direct call.
    on_professor_selected(GTK_COMBO_BOX(professor_combo), NULL);
}

/**
 * @brief Callback for the category shortcut buttons in the left navigation pane.
 * This function scrolls the main content area (`main_category_scrolled_window`)
 * to bring the corresponding category frame into view.
 *
 * @param button The GtkButton (shortcut button) that was clicked.
 * @param user_data User data (not used, but the target frame is stored on the button).
 */
void on_category_shortcut_clicked(GtkWidget *button, gpointer user_data) {
    (void)user_data; // Suppress unused parameter warning

    // Retrieve the target category frame widget that was stored in the button's data.
    GtkWidget *target_frame = g_object_get_data(G_OBJECT(button), "category-frame");

    if (target_frame) {
        g_print("Category shortcut clicked. Scrolling to frame: %s\n", gtk_frame_get_label(GTK_FRAME(target_frame)));
        // Get the adjustment object for the vertical scrollbar of the main content area.
        GtkAdjustment *vadjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(main_category_scrolled_window));

        // Declare a GtkAllocation struct to store the widget's allocation.
        GtkAllocation allocation;
        // Get the allocation of the target frame.
        gtk_widget_get_allocation(target_frame, &allocation);

        // Get the Y position from the allocation struct.
        gdouble target_y = (gdouble)allocation.y;

        // Set the new value of the vertical adjustment to scroll to the target Y position.
        gtk_adjustment_set_value(vadjustment, target_y);
    } else {
        g_warning("Target category frame not found for shortcut button.");
    }
}

/**
 * @brief Displays the configuration dialog, allowing users to edit professors and categories.
 * The dialog includes two tabs, one for professors and one for categories, each with
 * a text view and a save button.
 *
 * @param button The GtkButton that triggered the dialog.
 * @param user_data User data (not used in this function).
 */
void show_config_dialog(GtkWidget *button, gpointer user_data) {
    (void)button;    // Suppress unused parameter warning
    (void)user_data; // Suppress unused parameter warning

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Configurações",
                                                    NULL, // Parent window (NULL for transient_for later)
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Fechar", GTK_RESPONSE_CLOSE,
                                                    NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 500); // Set default size for config dialog

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(content_area), notebook);

    GError *error = NULL;
    gchar *file_content = NULL;

    // --- Professors Tab ---
    GtkWidget *professors_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(professors_vbox), 10);

    GtkWidget *professors_label = gtk_label_new("Lista de Professores (um por linha):");
    gtk_box_pack_start(GTK_BOX(professors_vbox), professors_label, FALSE, FALSE, 0);

    professors_text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(professors_text_view), GTK_WRAP_WORD);
    GtkWidget *prof_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(prof_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(prof_scrolled_window), professors_text_view);
    gtk_box_pack_start(GTK_BOX(professors_vbox), prof_scrolled_window, TRUE, TRUE, 5);

    // Load current professors into text view
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


    // --- Categories Tab ---
    GtkWidget *categories_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(categories_vbox), 10);

    GtkWidget *categories_label = gtk_label_new("Lista de Categorias (um por linha):");
    gtk_box_pack_start(GTK_BOX(categories_vbox), categories_label, FALSE, FALSE, 0);

    categories_text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(categories_text_view), GTK_WRAP_WORD);
    GtkWidget *cat_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cat_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(cat_scrolled_window), categories_text_view);
    gtk_box_pack_start(GTK_BOX(categories_vbox), cat_scrolled_window, TRUE, TRUE, 5);

    // Load current categories into text view
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
 * @brief Displays a help dialog with instructions on how to use the application.
 *
 * @param button The GtkButton that triggered the dialog.
 * @param user_data User data (not used in this function).
 */
void on_help_button_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK,
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
                                               "  * gnome-terminal (ou similar, para executar o script de backend em uma janela separada)\n");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// =============================================================================
// MAIN APPLICATION FUNCTION
// This is the entry point of the GTK+ application.
// =============================================================================

/**
 * @brief Main function to initialize GTK, create the main application window and GUI,
 * and start the GTK event loop. It also handles initial configuration loading
 * and application cleanup.
 *
 * @param argc The number of command-line arguments.
 * @param argv An array of command-line argument strings.
 * @return 0 on successful execution, 1 on error.
 */
int main(int argc, char *argv[]) {
    // Section: Locale Setup
    // Set the locale to the user's default. This is crucial for proper character encoding
    // handling by the C standard library, especially when dealing with accented characters
    // in filenames, metadata, and external process output (e.g., from `popen`).
    setlocale(LC_ALL, "");

    // Section: GTK Initialization
    gtk_init(&argc, &argv); // Initialize GTK+ toolkit.

    // Mutexs are removed as they are not needed for these global variables
    // given the GTK main loop and GObject lifecycle management.

    // Section: Ensure Configuration Directory Exists
    // Create the "config" directory if it doesn't already exist.
    g_mkdir_with_parents("config", 0755);
    // Removed: g_mkdir_with_parents("professores", 0755); as per user's specified structure
    // Ensure the final report directory exists
    g_mkdir_with_parents("final", 0755);


    // Section: Load Categories
    // Attempt to load categories from `config/categories.txt`.
    if (!load_categories_from_file("config/categories.txt")) {
        // If categories cannot be loaded, display a critical error message and exit.
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Erro: Não foi possível carregar as categorias do arquivo 'config/categories.txt'.\n"
                                                   "Por favor, crie este arquivo e adicione as categorias (uma por linha).");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return 1; // Exit with an error code.
    }

    // Section: Main Window Creation
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); // Create a new top-level window.
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 800); // Set default window size (width, height).
    // Connect the "destroy" signal of the window to `gtk_main_quit` to exit the application.
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10); // Main vertical box for window content.
    gtk_container_add(GTK_CONTAINER(window), main_vbox); // Add main_vbox to the window.
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 10); // Add 10px padding around main content.

    // Section: Professor Selection UI
    GtkWidget *professor_frame = gtk_frame_new("Selecione o Professor"); // Frame for professor selection.
    professor_combo = gtk_combo_box_text_new(); // Create a combo box for professor selection.
    gtk_widget_set_size_request(GTK_WIDGET(professor_combo), -1, 40); // Set minimum height for combo box.
    gtk_widget_set_hexpand(GTK_WIDGET(professor_combo), TRUE); // Allow horizontal expansion.
    gtk_widget_set_halign(GTK_WIDGET(professor_combo), GTK_ALIGN_FILL); // Fill horizontal space.
    gtk_widget_set_vexpand(GTK_WIDGET(professor_combo), TRUE); // Allow vertical expansion.
    gtk_widget_set_valign(GTK_WIDGET(professor_combo), GTK_ALIGN_FILL); // Fill vertical space.

    // Load professors into the combo box from `config/professores.txt`.
    if (!load_professors_from_file("config/professores.txt")) {
        // A warning is already printed by the function if loading fails.
        // The combo box will show "Nenhum Professor Disponível".
    }
    gtk_container_add(GTK_CONTAINER(professor_frame), professor_combo); // Add combo box to its frame.
    gtk_widget_set_hexpand(GTK_WIDGET(professor_frame), TRUE); // Frame expands horizontally.
    gtk_widget_set_halign(GTK_WIDGET(professor_frame), GTK_ALIGN_FILL); // Fill horizontal space.
    gtk_widget_set_vexpand(GTK_WIDGET(professor_frame), FALSE); // Frame itself shouldn't expand vertically too much.
    gtk_box_pack_start(GTK_BOX(main_vbox), professor_frame, FALSE, FALSE, 0); // Pack frame into main_vbox.

    // Section: Top Buttons (Configuration, Help, Reload Professor, Create Folder)
    GtkWidget *top_buttons_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); // Horizontal box for buttons.
    gtk_box_pack_start(GTK_BOX(main_vbox), top_buttons_hbox, FALSE, FALSE, 5); // Pack into main_vbox with padding.

    GtkWidget *config_btn = gtk_button_new_with_label("Configurações"); // Config button.
    g_signal_connect(config_btn, "clicked", G_CALLBACK(show_config_dialog), NULL); // Connect to config dialog.
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), config_btn, FALSE, FALSE, 0);

    GtkWidget *help_btn = gtk_button_new_with_label("Ajuda"); // Help button.
    g_signal_connect(help_btn, "clicked", G_CALLBACK(on_help_button_clicked), NULL); // Connect to help dialog.
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), help_btn, FALSE, FALSE, 0);

    GtkWidget *reload_professor_btn = gtk_button_new_with_label("Recarregar Professor"); // Reload button.
    g_signal_connect(reload_professor_btn, "clicked", G_CALLBACK(on_reload_professor_button_clicked), NULL); // Connect to reload callback.
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), reload_professor_btn, FALSE, FALSE, 0);

    GtkWidget *create_prof_folder_btn = gtk_button_new_with_label("Criar Pasta do Professor"); // NEW: Create Professor Folder button.
    g_signal_connect(create_prof_folder_btn, "clicked", G_CALLBACK(on_create_professor_folder_clicked), NULL); // Connect to new callback.
    gtk_box_pack_start(GTK_BOX(top_buttons_hbox), create_prof_folder_btn, FALSE, FALSE, 0);

    // Connect the "changed" signal of the professor combo box to its handler.
    // This will trigger the initial load of the selected professor's data.
    g_signal_connect(professor_combo, "changed", G_CALLBACK(on_professor_selected), NULL);

    // Section: Main Content Area (Horizontal Paned Window)
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); // Horizontal paned window for two panes.
    gtk_box_pack_start(GTK_BOX(main_vbox), hpaned, TRUE, TRUE, 0); // Allow hpaned to expand.

    // Left pane: Category Navigation (scrollable list of buttons).
    GtkWidget *nav_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nav_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(nav_scrolled_window, 400, -1); // Set width for navigation pane.
    gtk_paned_pack1(GTK_PANED(hpaned), nav_scrolled_window, FALSE, FALSE); // Pack into left pane.

    category_nav_list_box = gtk_list_box_new(); // List box for category shortcut buttons.
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(category_nav_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(nav_scrolled_window), category_nav_list_box); // Add list box to scrolled window.

    // Right pane: Main Category Content (all category frames in a single scrollable area).
    main_category_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(main_category_scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_paned_pack2(GTK_PANED(hpaned), main_category_scrolled_window, TRUE, FALSE); // Pack into right pane.

    category_content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10); // Vertical box to hold all category frames.
    gtk_container_add(GTK_CONTAINER(main_category_scrolled_window), category_content_vbox); // Add to scrolled window.
    gtk_container_set_border_width(GTK_CONTAINER(category_content_vbox), 5); // Padding inside content area.

    // Create and populate category sections (uses the new nav and content widgets).
    create_category_gui(category_nav_list_box, category_content_vbox);

    // Section: Bottom Buttons (Save, Generate, Preview Report)
    GtkWidget *bottom_buttons_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); // Horizontal box for buttons.
    gtk_widget_set_halign(bottom_buttons_hbox, GTK_ALIGN_CENTER); // Center buttons.
    gtk_box_pack_end(GTK_BOX(main_vbox), bottom_buttons_hbox, FALSE, FALSE, 10); // Pack at the end with padding.

    save_all_btn = gtk_button_new_with_label("Salvar Alterações"); // Save All button.
    g_signal_connect(save_all_btn, "clicked", G_CALLBACK(on_save_all_entries), NULL); // Connect to save callback.
    gtk_box_pack_start(GTK_BOX(bottom_buttons_hbox), save_all_btn, TRUE, TRUE, 0);

    GtkWidget *generate_btn = gtk_button_new_with_label("Gerar Relatório Profissional"); // Generate Report button.
    g_signal_connect(generate_btn, "clicked", G_CALLBACK(generate_report), NULL); // Connect to generate callback.
    gtk_box_pack_start(GTK_BOX(bottom_buttons_hbox), generate_btn, TRUE, TRUE, 0);

    preview_report_btn = gtk_button_new_with_label("Visualizar Relatório Profissional"); // Preview Report button.
    g_signal_connect(preview_report_btn, "clicked", G_CALLBACK(on_preview_report_pdf), NULL); // Connect to preview callback.
    gtk_box_pack_start(GTK_BOX(bottom_buttons_hbox), preview_report_btn, TRUE, TRUE, 0);
    gtk_widget_hide(preview_report_btn); // Initially hide it.

    // Section: Progress Bar and Status Label
    status_label = gtk_label_new("Pronto."); // Status label.
    gtk_widget_set_hexpand(status_label, TRUE); // Expand horizontally.
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0); // Align text to left.
    gtk_box_pack_end(GTK_BOX(main_vbox), status_label, FALSE, FALSE, 5); // Pack above progress bar.

    progress_bar = gtk_progress_bar_new(); // Progress bar.
    gchar *initial_progress_text = g_strdup_printf("%d%%", 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), initial_progress_text); // Initial text.
    SAFE_FREE(initial_progress_text); // Free the string after use.
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE); // Show text.
    gtk_widget_set_hexpand(progress_bar, TRUE); // Expand horizontally.
    gtk_box_pack_end(GTK_BOX(main_vbox), progress_bar, FALSE, FALSE, 5); // Pack at the very bottom.


    // Show all widgets and start the GTK main loop
    gtk_widget_show_all(window);
    gtk_main();

    // Cleanup: Free all dynamically allocated memory
    cleanup_category_gui(); // Cleans up sections and their entries
    // Free dynamically allocated global_categories strings and the array itself
    if (global_categories) {
        for (gint i = 0; i < global_num_categories; i++) {
            SAFE_FREE(global_categories[i]);
        }
        SAFE_FREE(global_categories);
    }
    // Free global_max_category_counters
    SAFE_FREE(global_max_category_counters);

    return 0;
}

