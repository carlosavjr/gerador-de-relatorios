#!/bin/bash

echo ""
echo "########################################"
echo "COLETANDO DADOS DE CATEGORIA DOS PDFs"
echo "########################################"
echo ""

# Get the directory where the script is initially executed
script_initial_dir=$(pwd)

# Define file paths using absolute paths for clarity and correctness
professores_file="$script_initial_dir/lista/professores.txt"
categories_to_count_file="$script_initial_dir/config/categories_to_count.txt"
output_data_file="$script_initial_dir/report_data.csv" # THIS IS THE CRUCIAL FIX: now an absolute path

# Define the fixed production folder name
producao_folder_name='Produção Docente'
# The metadata field to check for categories
category_field_name='Category' # Fixed to 'Category' as requested

# Check for input files
echo "Verificando se o arquivo de professores existe: $professores_file"
[ ! -f "$professores_file" ] && { echo "ERRO: Arquivo $professores_file não encontrado. Certifique-se de que 'lista/professores.txt' está no lugar correto."; exit 1; }
echo "Arquivo de professores encontrado."

echo "Verificando se o arquivo de categorias existe: $categories_to_count_file"
[ ! -f "$categories_to_count_file" ] && { echo "ERRO: Arquivo de categorias para contar $categories_to_count_file não encontrado. Certifique-se de que 'config/categories_to_count.txt' está no lugar correto."; exit 1; }
echo "Arquivo de categorias encontrado."

# Check if exiftool is installed
if ! command -v exiftool &> /dev/null; then
    echo "ERRO: 'exiftool' não encontrado. Por favor, instale-o para continuar."
    echo "  No Debian/Ubuntu: sudo apt-get install libimage-exiftool-perl"
    echo "  No macOS (Homebrew): brew install exiftool"
    exit 1
fi
echo "exiftool está instalado."

# Initialize counters for on-screen summary
declare -A category_counts # Associative array for category counts
total_pdfs_scanned=0
total_pdfs_no_category=0 # Counter for PDFs with no recognized category

# NEW: Declare associative array for category aliases
declare -A category_aliases

# Read categories to count from config file into an array and initialize their counts
# Also populate the category_aliases associative array
categories_to_check=() # Re-initialize as a regular array
echo "Lendo categorias e aliases de '$categories_to_count_file'..."
while IFS=',' read -r category_name alias_name || [ -n "$category_name" ]; do
    # Trim whitespace from category_name
    category_name=$(echo "$category_name" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    # Trim quotes and whitespace from alias_name
    alias_name=$(echo "$alias_name" | sed "s/^'//;s/'$//;s/^[[:space:]]*//;s/[[:space:]]*$//")

    if [ -n "$category_name" ]; then
        categories_to_check+=("$category_name") # Add raw category name to check array
        if [ -n "$alias_name" ]; then
            category_aliases["$category_name"]="$alias_name" # Store alias
            echo "  - Categoria: '$category_name', Alias: '$alias_name'"
        else
            category_aliases["$category_name"]="$category_name" # Use category name as alias if none provided
            echo "  - Categoria: '$category_name', Sem Alias (usando o próprio nome)"
        fi
        category_counts["$category_name"]=0 # Initialize count for each category
    fi
done < "$categories_to_count_file"

if [ ${#categories_to_check[@]} -eq 0 ]; then
    echo "AVISO: Nenhuma categoria definida em '$categories_to_count_file'. PDFs serão marcados como 'NoCategory' se não houver categorias para verificar."
fi

# Clear previous output data file and write header
echo "Criando/limpando o arquivo de saída: '$output_data_file'"
# UPDATED: Added CategoryAlias to the CSV header
echo "Professor,PDF_File,Categories,CategoryAlias" > "$output_data_file"
echo "Coletando dados e salvando em '$output_data_file'..."
echo ""

# Process each professor from the professors file
echo "Iniciando o processamento dos professores do arquivo '$professores_file'."
while IFS= read -r professor || [ -n "$professor" ]; do
    # Trim whitespace and skip empty lines
    professor=$(echo "$professor" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    [ -z "$professor" ] && continue

    echo "--- Processando professor: '$professor' ---"

    # Safely navigate to professor's production folder
    current_professor_path="$script_initial_dir/$professor/$producao_folder_name" # Use absolute path for cd
    echo "Tentando entrar na pasta: '$current_professor_path'"
    if cd "$current_professor_path"; then # Removed 2>/dev/null to show errors
        echo "Sucesso: Entrou na pasta: '$(pwd)'" # Confirm current working directory

        # Read PDF file paths into an array to avoid subshell issues
        mapfile -t pdf_list < <(find . -maxdepth 1 -type f -name "*.pdf")

        if [ ${#pdf_list[@]} -eq 0 ]; then
            echo "Nenhum PDF encontrado na pasta '$(pwd)'."
        else
            echo "Encontrados ${#pdf_list[@]} PDFs na pasta '$(pwd)'."
        fi

        for pdf_file in "${pdf_list[@]}"; do
            ((total_pdfs_scanned++)) # Increment total scanned PDFs
            pdf_name=$(basename "$pdf_file")
            echo "  Processando PDF: '$pdf_name'"
            category_content=$(exiftool -s3 -c -q -T -"$category_field_name" "$pdf_file" | tr '[:upper:]' '[:lower:]')

            found_categories=()
            found_category_aliases=() # NEW: To store aliases of found categories
            if [ -n "$(echo "$category_content" | xargs)" ]; then
                for category in "${categories_to_check[@]}"; do
                    if echo "$category_content" | grep -q "\b$category\b"; then
                        found_categories+=("$category")
                        # Add the alias to the new array
                        found_category_aliases+=("${category_aliases["$category"]}")
                        ((category_counts["$category"]++)) # Increment category specific counter
                    fi
                done
            fi

            # Format categories for CSV output: comma-separated if found, "NoCategory" otherwise
            if [ ${#found_categories[@]} -gt 0 ]; then
                # Join array elements with | for CSV field
                joined_categories=$(IFS='|'; echo "${found_categories[*]}")
                joined_aliases=$(IFS='|'; echo "${found_category_aliases[*]}") # Join aliases
                echo "    Categoria(s) encontrada(s): '$joined_categories' (Aliases: '$joined_aliases'). Registrando em CSV."
                # UPDATED: Added joined_aliases to the CSV output
                echo "$professor,\"$pdf_name\",\"$joined_categories\",\"$joined_aliases\"" >> "$output_data_file"
            else
                echo "    Nenhuma categoria reconhecida encontrada. Registrando 'NoCategory' em CSV."
                ((total_pdfs_no_category++)) # Increment no category counter
                # UPDATED: Added "NoCategory" for alias column
                echo "$professor,\"$pdf_name\",\"NoCategory\",\"NoCategory\"" >> "$output_data_file"
            fi
        done
        cd "$script_initial_dir" # Go back to the initial directory for the next professor
    else
        echo "ERRO: Não foi possível entrar na pasta '$current_professor_path'. Verifique o caminho e as permissões."
    fi
    echo "" # Add a blank line for better readability between professors
done < "$script_initial_dir/lista/professores.txt" # Use absolute path for reading professors file

echo "########################################"
echo "COLETA DE DADOS CONCLUÍDA."
echo "Os dados foram salvos em '$output_data_file'."
echo "Verifique o conteúdo do arquivo com: cat '$output_data_file'"
echo "########################################"
echo ""

# Display the summary on screen using category aliases
echo "########################################"
echo "RESUMO GERAL POR CATEGORIA"
echo "########################################"
printf "%-35s: %2d\n" "Total de arquivos PDF escaneados" $total_pdfs_scanned
echo ""
echo "Contagem por Categoria ($category_field_name):"
for category in "${!category_counts[@]}"; do
    # Use the alias for display, fall back to raw name if no alias defined
    display_name="${category_aliases["$category"]:-$category}"
    printf "%-35s: %2d\n" "  '$display_name'" "${category_counts["$category"]}"
done
printf "%-35s: %2d\n" "  'Sem categoria reconhecida'" $total_pdfs_no_category
echo "########################################"
