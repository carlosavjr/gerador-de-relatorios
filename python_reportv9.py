import pandas as pd
import matplotlib.pyplot as plt
import os
import numpy as np # Added numpy for mathematical operations

def escape_latex_special_chars(text):
    """Escapes common LaTeX special characters in a string."""
    # Order matters: escape backslash first
    text = str(text) # Ensure text is a string
    text = text.replace('\\', r'\textbackslash{}')
    text = text.replace('&', r'\&')
    text = text.replace('%', r'\%')
    text = text.replace('$', r'\$')
    text = text.replace('#', r'\#')
    text = text.replace('{', r'\{')
    text = text.replace('}', r'\}')
    text = text.replace('~', r'\textasciitilde{}')
    text = text.replace('^', r'\textasciicircum{}')
    text = text.replace('_', r'\_')
    return text

# Global variables to store mappings and ordered list
alias_to_display_name_map = {}
display_name_to_sort_key_map = {}
ordered_display_categories_list = []

def load_custom_category_order(file_path):
    """
    Loads custom category order and sort keys from a text file.
    Each line in the file is expected to be in the format: alias,'Display Name',sort_key
    Updates global maps and lists.
    """
    global alias_to_display_name_map, display_name_to_sort_key_map, ordered_display_categories_list
    
    alias_to_display_name_map = {}
    display_name_to_sort_key_map = {}
    temp_ordered_items = [] # To store (sort_key, display_name) tuples

    if not os.path.exists(file_path):
        print(f"Error: Custom category order file '{file_path}' not found. Categories will be sorted alphabetically (default fallback).")
        # If file not found, maps remain empty, and get_category_sort_key will use default 1000.
        # ordered_display_categories_list will be populated with all unique display names found in data, sorted by default key.
        return 

    with open(file_path, 'r', encoding='utf-8') as f:
        for line in f:
            stripped_line = line.strip()
            if stripped_line:
                parts = stripped_line.split(',', 2) # Split into max 3 parts
                if len(parts) == 3:
                    alias = parts[0].strip()
                    display_name = parts[1].strip().strip("'")
                    try:
                        sort_key = int(parts[2].strip())
                        
                        alias_to_display_name_map[alias] = display_name
                        display_name_to_sort_key_map[display_name] = sort_key
                        temp_ordered_items.append((sort_key, display_name))
                    except ValueError:
                        print(f"Warning: Invalid sort_key '{parts[2]}' for category '{display_name}' in line: '{stripped_line}'. This category will be sorted with default priority.")
                        # Assign a default high priority if sort_key is invalid
                        alias_to_display_name_map[alias] = display_name # Still map alias to display name
                        display_name_to_sort_key_map[display_name] = 1000
                        temp_ordered_items.append((1000, display_name))
                else:
                    print(f"Warning: Line '{stripped_line}' in categories_to_count.txt does not match expected format (alias,'Display Name',sort_key). This category will be sorted with default priority.")
                    # If format is incorrect, try to at least map alias to display name if possible
                    fallback_parts = stripped_line.split(',', 1)
                    if len(fallback_parts) == 2:
                        alias = fallback_parts[0].strip()
                        display_name = fallback_parts[1].strip().strip("'")
                        alias_to_display_name_map[alias] = display_name
                        display_name_to_sort_key_map[display_name] = 1000 # Assign default high priority
                        temp_ordered_items.append((1000, display_name))
                    else:
                        # If even this fails, just use the whole line as display name and assign high priority
                        display_name = stripped_line
                        alias_to_display_name_map[display_name] = display_name # Map to itself
                        display_name_to_sort_key_map[display_name] = 1000
                        temp_ordered_items.append((1000, display_name))


    # Sort the items based on their sort_key to get the final ordered list of display names
    ordered_display_categories_list[:] = [item[1] for item in sorted(temp_ordered_items, key=lambda x: x[0])]
    
    # Add "Sem Categoria" to the end of the ordered list if it's not already there
    if "Sem Categoria" not in ordered_display_categories_list:
        ordered_display_categories_list.append("Sem Categoria")
        display_name_to_sort_key_map["Sem Categoria"] = 9999 # Ensure it has the highest sort key

def get_category_sort_key(display_name):
    """
    Returns the sort key for a given display name.
    """
    # If the display_name is not found in our loaded map, assign a default high priority.
    # This handles cases where categories exist in data but not in categories_to_count.txt
    return display_name_to_sort_key_map.get(display_name, 1000)


def generate_latex_report(data_file="report_data.csv", output_tex_file="category_report.tex",
                          histogram_file="category_histogram.pdf",
                          professors_9plus_circular_graph_file="professors_9plus_circular.pdf",
                          global_categories_circular_graph_file="global_categories_circular.pdf",
                          categories_order_file="categories_to_count.txt"):
    """
    Generates a LaTeX report from PDF category data, including per-professor listings,
    a global histogram, and two circular graphs, with a custom ABNT cover page.

    Args:
        data_file (str): Path to the CSV file generated by collect_category_data.sh.
        output_tex_file (str): Name of the output LaTeX file.
        histogram_file (str): Name of the output histogram image file (PDF).
        professors_9plus_circular_graph_file (str): Name of the output circular graph image file for 9+ professors (PDF).
        global_categories_circular_graph_file (str): Name of the output circular graph image file for global categories (PDF).
        categories_order_file (str): Path to the file defining the custom order of categories.
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))

    data_file_abs = os.path.join(script_dir, data_file)
    professors_list_file_abs = os.path.join(script_dir, "lista", "professores.txt")
    
    # Corrected path to categories_to_count.txt, assuming it's in a 'config' subdirectory
    categories_order_file_abs = os.path.join(script_dir, 'config', categories_order_file)

    # Load the custom category order and the ordered list of display names
    load_custom_category_order(categories_order_file_abs) # Call the function to populate global variables
    print(f"DEBUG: Custom category order map (alias to display name): {alias_to_display_name_map}")
    print(f"DEBUG: Display name to sort key map: {display_name_to_sort_key_map}")
    print(f"DEBUG: Ordered display categories list: {ordered_display_categories_list}")


    if not os.path.exists(data_file_abs):
        print(f"Erro: Arquivo de dados '{data_file_abs}' não encontrado. Por favor, execute 'collect_category_data.sh' primeiro.")
        df = pd.DataFrame(columns=['Professor', 'PDF_File', 'Categories', 'CategoryAlias'])
    else:
        df = pd.read_csv(data_file_abs)

    global_total_productions = df.shape[0]

    all_professors_from_list = []
    if os.path.exists(professors_list_file_abs):
        with open(professors_list_file_abs, 'r', encoding='utf-8') as f:
            for line in f:
                stripped_line = line.strip()
                if stripped_line:
                    all_professors_from_list.append(stripped_line)
        total_professors = len(all_professors_from_list)
        print(f"DEBUG: Total professors read from '{professors_list_file_abs}': {total_professors}")
    else:
        print(f"Aviso: Arquivo '{professors_list_file_abs}' não encontrado. O total de professores será baseado apenas nos dados de PDFs.")
        all_professors_from_list = df['Professor'].unique().tolist()
        total_professors = len(all_professors_from_list)
        print(f"DEBUG: Total professors (fallback from CSV): {total_professors}")

    all_display_categories = []
    for _, row in df.iterrows():
        # Use the 'Categories' column for the raw alias
        raw_aliases_str = row['Categories'] 
        if raw_aliases_str != "NoCategory":
            for raw_alias_item in raw_aliases_str.split('|'):
                # Look up the display name using the raw alias
                display_name = alias_to_display_name_map.get(raw_alias_item, raw_alias_item) # Fallback to raw alias if not found
                all_display_categories.append(display_name)
        else:
            all_display_categories.append("Sem Categoria")

    # Create global category counts for plotting and data retrieval
    global_category_counts_dict = pd.Series(all_display_categories).value_counts().to_dict()


    professor_data = {prof: {'total_productions': 0, 'category_counts': {} } for prof in all_professors_from_list}

    for professor in all_professors_from_list: # Iterate through all professors from the list, not just those in df
        professor_df = df[df['Professor'] == professor]
        individual_total_productions = professor_df.shape[0]

        professor_category_counts = {}
        for _, row in professor_df.iterrows():
            # Use the 'Categories' column for the raw alias
            raw_aliases_str = row['Categories']
            if raw_aliases_str != "NoCategory":
                for raw_alias_item in raw_aliases_str.split('|'):
                    # Look up the display name using the raw alias
                    display_name = alias_to_display_name_map.get(raw_alias_item, raw_alias_item) # Fallback to raw alias if not found
                    professor_category_counts[display_name] = professor_category_counts.get(display_name, 0) + 1
            else:
                professor_category_counts["Sem Categoria"] = professor_category_counts.get("Sem Categoria", 0) + 1

        professor_data[professor] = {
            'total_productions': individual_total_productions,
            'category_counts': professor_category_counts
        }

    global_professors_with_9_plus_productions = 0
    for professor in all_professors_from_list:
        if professor_data[professor]['total_productions'] >= 9:
            global_professors_with_9_plus_productions += 1

    print(f"DEBUG: Professors with 9+ productions: {global_professors_with_9_plus_productions}")
    print(f"DEBUG: Professors with less than 9 productions (including 0): {total_professors - global_professors_with_9_plus_productions}")


    # For the histogram, we use the global_category_counts_dict and sort its keys for plotting
    histogram_data_sorted = sorted(global_category_counts_dict.items(), key=lambda item: get_category_sort_key(item[0]))
    histogram_categories = [item[0] for item in histogram_data_sorted]
    histogram_counts = [item[1] for item in histogram_data_sorted]

    if histogram_categories: # Check if there are categories to plot
        plt.figure(figsize=(10, 6))
        plt.bar(histogram_categories, histogram_counts, color='skyblue')
        plt.xlabel('Categoria', fontsize=14)
        plt.ylabel('Número de Produções', fontsize=14)
        plt.xticks(rotation=45, ha='right', fontsize=14)
        plt.yticks(fontsize=14)
        plt.tight_layout()
        plt.savefig(histogram_file, bbox_inches='tight')
        plt.close()
        print(f"Histograma gerado: {histogram_file}")
    else:
        print("Aviso: Não há dados de categorias globais para gerar o histograma.")


    professors_less_than_9 = total_professors - global_professors_with_9_plus_productions

    print(f"DEBUG (Before Pie Chart): global_professors_with_9_plus_productions = {global_professors_with_9_plus_productions}")
    print(f"DEBUG (Before Pie Chart): professors_less_than_9 = {professors_less_than_9}")


    if total_professors > 0:
        labels = [f'Professores com 9+ Produções ({global_professors_with_9_plus_productions})',
                  f'Professores com Menos de 9 Produções ({professors_less_than_9})']
        sizes = [global_professors_with_9_plus_productions, professors_less_than_9]
        colors = ['lightgreen', 'lightcoral']
        explode = (0.1, 0)

        # Create figure and axes for side-by-side layout
        fig, ax = plt.subplots(figsize=(10, 8)) # Increased width for side-by-side
        wedges, texts, autotexts = ax.pie(sizes, explode=explode, colors=colors,
                                           autopct='%1.1f%%', shadow=True, startangle=140, textprops={'fontsize': 14},
                                           radius=1.2)
        ax.axis('equal')
        # Legend positioned to the right of the chart
        ax.legend(wedges, labels, title="Legenda", loc="center left", bbox_to_anchor=(1, 0.5), fontsize=12)
        plt.savefig(professors_9plus_circular_graph_file, bbox_inches='tight')
        plt.close()
        print(f"Gráfico circular de 9+ produções gerado: {professors_9plus_circular_graph_file}")
    else:
        print("Aviso: Não há professores para gerar o gráfico circular de 9+ produções.")

    if global_category_counts_dict: # Use global_category_counts_dict for pie chart data
        # Prepare data for the pie chart, including all categories
        pie_labels = []
        pie_sizes = []
        
        # Ensure pie chart labels and sizes are in the desired order
        for cat_display_name in ordered_display_categories_list:
            count = global_category_counts_dict.get(cat_display_name, 0)
            if count > 0:
                pie_labels.append(f"{escape_latex_special_chars(cat_display_name)} ({count})")
                pie_sizes.append(count)
        
        # Add "Sem Categoria" if it exists and has counts (and not already in ordered_display_categories_list)
        if "Sem Categoria" in global_category_counts_dict and global_category_counts_dict["Sem Categoria"] > 0 and "Sem Categoria" not in ordered_display_categories_list:
            count = global_category_counts_dict["Sem Categoria"]
            pie_labels.append(f"{escape_latex_special_chars('Sem Categoria')} ({count})")
            pie_sizes.append(count)

        # Proceed with plotting only if there are slices to show
        if pie_sizes:
            explode_sizes = [0] * len(pie_sizes)
            if len(pie_sizes) > 0:
                largest_slice_index = pie_sizes.index(max(pie_sizes)) # Find index of largest slice
                explode_sizes[largest_slice_index] = 0.1

            # Define autopct function to hide percentages less than 1%
            def autopct_format(pct):
                if pct >= 1: # Only show percentage if it's 1% or more
                    return '{:.1f}%'.format(pct)
                return '' # Return empty string for percentages less than 1%


            # Create figure and axes for side-by-side layout
            fig, ax = plt.subplots(figsize=(12, 10)) # Increased width for side-by-side
            wedges, texts, autotexts = ax.pie(pie_sizes, explode=explode_sizes, autopct=autopct_format, # Use the custom autopct function
                                               shadow=True, startangle=140, pctdistance=0.85, textprops={'fontsize': 14},
                                               radius=1.2)
            ax.axis('equal')

            # Removed manual adjustment of percentage label positions

            # Legend positioned to the right of the chart
            ax.legend(wedges, pie_labels, title="Legenda", loc="center left", bbox_to_anchor=(1, 0.5), fontsize=12)
            plt.savefig(global_categories_circular_graph_file, bbox_inches='tight')
            plt.close()
            print(f"Gráfico circular de categorias globais gerado: {global_categories_circular_graph_file}")
        else:
            print("Aviso: Não há dados de categorias globais (after filtering) para gerar o gráfico circular.")
    else:
        print("Aviso: Não há dados de categorias globais para gerar o gráfico circular.")


    # --- Generate LaTeX Report ---
    latex_content = []
    # Explicitly set document class to a4paper
    latex_content.append(r"\documentclass[a4paper]{abntex2}")
    latex_content.append(r"\usepackage[utf8]{inputenc}")
    latex_content.append(r"\usepackage[T1]{fontenc}")
    latex_content.append(r"\usepackage{graphicx}")
    latex_content.append(r"\usepackage{amsmath}")
    latex_content.append(r"\usepackage{amssymb}")
    latex_content.append(r"\usepackage{float}") # Uncommented float package
    latex_content.append(r"\usepackage{fancyhdr}") # Added for custom headers/footers
    latex_content.append(r"\usepackage{lipsum}") # Added for dummy text if needed for testing layout
    latex_content.append(r"\usepackage{hyperref}") # Ensure hyperref is loaded after other packages that might define commands it uses
    latex_content.append(r"\usepackage{longtable}") # Added for tables that span multiple pages
    latex_content.append(r"\usepackage{booktabs}") # For better table rules (e.g., \toprule, \midrule, \bottomrule)
    latex_content.append(r"\usepackage{array}") # For column formatting
    latex_content.append(r"\usepackage{tabularx}") # Added for flexible table width
    latex_content.append(r"\usepackage{geometry}") # Added for page size control
    latex_content.append(r"\usepackage{afterpage}") # Added for single-page geometry changes

    # Explicitly set A4 geometry for the main document with standard ABNT margins
    latex_content.append(r"\geometry{a4paper, left=3cm, right=2cm, top=3cm, bottom=2cm}")

    # Define new column types for tabularx in the preamble
    # L for left-aligned X column, C for centered X column
    latex_content.append(r"\newcolumntype{L}{>{\raggedright\arraybackslash}X}")
    latex_content.append(r"\newcolumntype{C}{>{\centering\arraybackslash}X}")

    latex_content.append(r"\renewcommand{\thesection}{\arabic{section}}")

    latex_content.append(r"\begin{document}")

    # Define the fancyhdr style for the link
    latex_content.append(r"\fancyhf{}") # Clear all header and footer fields
    latex_content.append(r"\fancyhead[R]{\hyperlink{toc_start}{[Voltar ao Sumário]}}") # Right header

    # Cover page
    latex_content.append(r"\thispagestyle{empty}") # Ensure cover page has no header/footer
    latex_content.append(r"\begin{center}")
    latex_content.append(r"%\vspace*{1cm}")
    latex_content.append(r"    \includegraphics[width=0.3\textwidth]{~/docentes/logo_ufpel.png}\\")
    latex_content.append(r"    \vspace{1cm}") # Changed from 2cm to 1cm
    latex_content.append(r"    {\LARGE \textbf{UNIVERSIDADE FEDERAL DE PELOTAS}}\\")
    latex_content.append(r"    \vspace{7cm}") # Changed from 6cm to 7cm
    latex_content.append(r"    {\LARGE \textbf{Relatório de Categorias de Produção Docente de 2022 a 2024}}")
    latex_content.append(r"    \vfill")
    latex_content.append(r"    {\large \textbf{PELOTAS - RS}}\\")
    latex_content.append(r"    {\large \textbf{\the\year}}")
    latex_content.append(r"\end{center}")
    latex_content.append(r"\newpage")

    # Table of Contents page
    latex_content.append(r"\thispagestyle{empty}") # Make sure no header/footer on ToC page itself
    latex_content.append(r"\hypertarget{toc_start}{}") # Using \hypertarget for a direct link to the start of the TOC
    latex_content.append(r"\tableofcontents*")
    latex_content.append(r"\listoffigures") # Add list of figures
    latex_content.append(r"\listoftables") # Add list of tables

    latex_content.append(r"\newpage")

    latex_content.append(r"\textual")
    # Removed: latex_content.append(r"\setcounter{section}{0}") # Let abntex2 handle section numbering
    latex_content.append(r"\pagestyle{fancy}") # Apply fancy pagestyle AFTER ToC and textual content starts

    # The link will now be automatically placed by fancyhdr in the header on subsequent pages
    latex_content.append(r"\section{Estatísticas Globais por Categoria}")
    latex_content.append(r"Este relatório apresenta a contagem de Produções por categoria de metadados em toda a produção docente.")

    latex_content.append(r"\subsection{Proporção de Professores com 9+ Produções}")
    latex_content.append(r"A proporção de professores que atendem ao critério de 9 ou mais Produções é visualizada no gráfico circular abaixo (Figura \ref{fig:professors_circular}).")
    latex_content.append(r"\begin{figure}[H]") # Changed h! to H
    latex_content.append(r"    \caption{Proporção de Professores com 9+ Produções.}") # Caption moved up, added dot
    latex_content.append(r"    \centering")
    latex_content.append(f"    \\includegraphics[width=12cm]{{{professors_9plus_circular_graph_file}}}")
    latex_content.append(r"    \label{fig:professors_circular}")
    latex_content.append(r"    \par\vspace{0.2cm}\noindent\textbf{Fonte:} Coordenação do Curso de Licenciatura em Física, 2025.") # Modified source line, added dot
    latex_content.append(r"\end{figure}")
    # Commented out the newpage command as requested
    # latex_content.append(r"\newpage")

    latex_content.append(r"\subsection{Distribuição Global de Categorias}")
    latex_content.append(r"A distribuição percentual de todas as categorias de Produções é visualizada no gráfico circular abaixo (Figura \ref{fig:global_categories_circular}).")
    latex_content.append(r"\begin{figure}[H]") # Changed h! to H
    latex_content.append(r"    \caption{Distribuição Percentual Global de Categorias de Produções.}") # Caption moved up, added dot
    latex_content.append(r"    \centering")
    latex_content.append(f"    \\includegraphics[width=12cm]{{{global_categories_circular_graph_file}}}")
    latex_content.append(r"    \label{fig:global_categories_circular}")
    latex_content.append(r"    \par\vspace{0.2cm}\noindent\textbf{Fonte:} Coordenação do Curso de Licenciatura em Física, 2025.") # Modified source line, added dot
    latex_content.append(r"\end{figure}")

    latex_content.append(r"\subsection{Histograma de Categorias}")
    latex_content.append(r"A distribuição das categorias encontradas nas Produções é visualizada no histograma abaixo (Figura \ref{fig:histogram}).")
    latex_content.append(r"\begin{figure}[H]") # Changed h! to H
    latex_content.append(r"    \caption{Distribuição Global de Categorias de Produções.}") # Caption moved up, added dot
    latex_content.append(r"    \centering")
    latex_content.append(f"    \\includegraphics[width=12cm]{{{histogram_file}}}")
    latex_content.append(r"    \label{fig:histogram}")
    latex_content.append(r"    \par\vspace{0.2cm}\noindent\textbf{Fonte:} Coordenação do Curso de Licenciatura em Física, 2025.") # Modified source line, added dot
    latex_content.append(r"\end{figure}")

    latex_content.append(r"\subsection{Contagem Detalhada por Categoria}")
    
    global_items = []
    # Iterate through the pre-defined ordered list of display categories
    for category_display_name in ordered_display_categories_list:
        count = global_category_counts_dict.get(category_display_name, 0)
        if count > 0:
            latex_category_alias = escape_latex_special_chars(category_display_name)
            producao_text = "Produção" if count == 1 else "Produções"
            global_items.append(f"    \\item \\textbf{{{latex_category_alias}}}: {count} {producao_text}")

    # Add "Sem Categoria" if it exists and has counts (and not already in ordered_display_categories_list)
    if "Sem Categoria" in global_category_counts_dict and global_category_counts_dict["Sem Categoria"] > 0 and "Sem Categoria" not in ordered_display_categories_list:
        count = global_category_counts_dict["Sem Categoria"]
        latex_category_alias = escape_latex_special_chars("Sem Categoria")
        producao_text = "Produção" if count == 1 else "Produções"
        global_items.append(f"    \\item \\textbf{{{latex_category_alias}}}: {count} {producao_text}")

    if global_items:
        latex_content.append(r"\begin{itemize}")
        latex_content.extend(global_items)
        latex_content.append(r"\end{itemize}")
    else:
        latex_content.append(r"Nenhuma categoria de Produção encontrada para o total global.")

    # Conditional for "Total Global de Produções"
    global_producao_text = "Produção" if global_total_productions == 1 else "Produções"
    latex_content.append(f"\\noindent\\textbf{{Total Global de Produções}}: {global_total_productions} {global_producao_text}.\\\\")
    latex_content.append(f"\\noindent\\textbf{{Total de Professores}}: {total_professors} Professores.\\\\")
    latex_content.append(f"\\noindent\\textbf{{Total de Professores com 9+ Produções}}: {global_professors_with_9_plus_productions} Professores.")
    latex_content.append(r"\newpage")

    latex_content.append(r"\section{Contagem de Categorias por Professor}")
    latex_content.append(r"Esta seção detalha a contagem de Produções por categoria para cada professor.")

    for professor in all_professors_from_list:
        data = professor_data.get(professor, {'total_productions': 0, 'category_counts': {}})
        latex_professor = escape_latex_special_chars(professor)
        latex_content.append(f"\\subsection{{{latex_professor}}}")

        criterio_text = "Critério de 9+ Produções"
        if data['total_productions'] >= 9:
            latex_content.append(f"\\noindent\\textbf{{{criterio_text}}}: SIM.")
        else:
            latex_content.append(f"\\noindent\\textbf{{{criterio_text}}}: NÃO.")

        professor_items = []
        if not data['category_counts'] and data['total_productions'] == 0:
            latex_content.append(r"Nenhuma Produção encontrada para este professor com categorias reconhecidas.")
        else:
            # Iterate through the pre-defined ordered list of display categories for per-professor counts
            for category_display_name in ordered_display_categories_list:
                count = data['category_counts'].get(category_display_name, 0)
                if count > 0: # Only add if count is greater than 0
                    latex_category_alias = escape_latex_special_chars(category_display_name)
                    producao_text = "Produção" if count == 1 else "Produções"
                    professor_items.append(f"    \\item \\textbf{{{latex_category_alias}}}: {count} {producao_text}")

            if "Sem Categoria" in data['category_counts'] and data['category_counts']["Sem Categoria"] > 0 and "Sem Categoria" not in ordered_display_categories_list:
                count = data['category_counts']["Sem Categoria"]
                latex_category_alias = escape_latex_special_chars("Sem Categoria")
                producao_text = "Produção" if count == 1 else "Produções"
                professor_items.append(f"    \\item \\textbf{{{latex_category_alias}}}: {count} {producao_text}")

            if professor_items:
                latex_content.append(r"\begin{itemize}")
                latex_content.extend(professor_items)
                latex_content.append(r"\end{itemize}")
            else:
                # This case means data['total_productions'] > 0 but no recognized categories with counts > 0
                latex_content.append(r"Nenhuma categoria de Produção reconhecida com contagem maior que zero para este professor.")

        # Conditional for "Total de Produções" per professor
        professor_producao_text = "Produção" if data['total_productions'] == 1 else "Produções"
        latex_content.append(f"\\noindent\\textbf{{Total de Produções}}: {data['total_productions']} {professor_producao_text}.")
        latex_content.append(r"\vspace{0.5cm}")
    latex_content.append(r"\newpage")

    # --- New Section: Detailed Production Table by Professor and Category ---
    latex_content.append(r"\clearpage") # Ensure all previous content is flushed before changing geometry
    latex_content.append(r"\afterpage{") # Start afterpage block
    latex_content.append(r"    \newgeometry{a3paper, landscape, margin=1cm}") # Apply A3 landscape for this page
    # The section and its content are now inside the afterpage block
    latex_content.append(r"    \section{Tabela Detalhada de Produções por Professor e Categoria}") # Changed to \section to include in ToC
    latex_content.append(r"    \label{sec:detailed_table}") # Add a label for this section, immediately after the section command
    latex_content.append(r"    Esta tabela apresenta a contagem de Produções por categoria para cada professor individualmente.")
    
    # Filter out "Sem Categoria" for the table columns
    table_display_categories = [cat for cat in ordered_display_categories_list if cat != "Sem Categoria"]
    
    # Define table columns: first column for Professor, then one column for each category
    # 'L' for left-aligned X column, 'C' for centered X column, 'c' for fixed centered
    table_column_format = "L|" + "C" * len(table_display_categories) + "|c" # Professor | Cat1 | Cat2 | ... | Total
    
    latex_content.append(r"    \begin{table}[H]")
    latex_content.append(r"        \caption{Contagem de Produções por Professor e Categoria.}")
    latex_content.append(r"        \centering")
    latex_content.append(r"        \tiny") # Reverted to \tiny font for the table
    latex_content.append(f"        \\begin{{tabularx}}{{\\textwidth}}{{{table_column_format}}}") # Use tabularx with textwidth
    latex_content.append(r"            \toprule")
    
    # Table Header Row with horizontal category names (no \rotcell)
    header_professor = "\\textbf{Professor}"
    header_categories = [f"\\textbf{{{escape_latex_special_chars(cat)}}}" for cat in table_display_categories]
    header_total = "\\textbf{Total}"
    header_row = [header_professor] + header_categories + [header_total]
    latex_content.append("            " + " & ".join(header_row) + r" \\")
    latex_content.append(r"            \midrule")

    # Table Content Rows
    for professor in all_professors_from_list:
        data = professor_data.get(professor, {'total_productions': 0, 'category_counts': {}})
        row_data = [escape_latex_special_chars(professor)]
        
        # Populate data for categories, excluding "Sem Categoria"
        for category_display_name in table_display_categories:
            count = data['category_counts'].get(category_display_name, 0)
            row_data.append(str(count))
        
        row_data.append(str(data['total_productions'])) # Add total productions for the professor
        latex_content.append("            " + " & ".join(row_data) + r" \\")
        latex_content.append(r"            \hline") # Added horizontal line between rows
    
    # Add the "Total per Category" row
    latex_content.append(r"            \midrule") # Line before the total row
    total_category_row_data = ["\\textbf{Total por Categoria}"]
    for category_display_name in table_display_categories:
        total_count_for_category = sum(professor_data[prof]['category_counts'].get(category_display_name, 0) for prof in all_professors_from_list)
        total_category_row_data.append(f"\\textbf{{{total_count_for_category}}}")
    total_category_row_data.append(f"\\textbf{{{global_total_productions}}}") # Global total productions
    latex_content.append("            " + " & ".join(total_category_row_data) + r" \\")
    
    latex_content.append(r"            \bottomrule")
    latex_content.append(r"        \end{tabularx}") 
    latex_content.append(r"        \par\vspace{0.2cm}\noindent\textbf{Fonte:} Coordenação do Curso de Licenciatura em Física, 2025.")
    latex_content.append(r"        \label{tab:professor_category_counts}")
    latex_content.append(r"    \end{table}")
    latex_content.append(r"    \clearpage") # Essential to flush the A3 page content before restoring geometry
    latex_content.append(r"    \restoregeometry") # Restore original page size for subsequent pages
    latex_content.append(r"}") # End of \afterpage block
    latex_content.append(r"\newpage") # Ensure next content starts on a new page (standard A4)


    latex_content.append(r"\end{document}")

    with open(output_tex_file, "w", encoding="utf-8") as f:
        f.write("\n".join(latex_content))
    print(f"Relatório LaTeX gerado: {output_tex_file}")

    print("\nPara compilar o relatório LaTeX:")
    print(f"1. Certifique-se de ter uma distribuição LaTeX instalada (ex: TeX Live, MiKTeX).")
    print(f"2. Abra um terminal no mesmo diretório dos arquivos '{output_tex_file}' e '{histogram_file}'.")
    print(f"3. Execute o comando: pdflatex {output_tex_file}")
    print(f"4. Se necessário (para sumário, referências), execute 'pdflatex {output_tex_file}' mais uma ou duas vezes.")
    print(f"5. Um arquivo '{output_tex_file.replace('.tex', '.pdf')}' será gerado.")


if __name__ == "__main__":
    generate_latex_report()

