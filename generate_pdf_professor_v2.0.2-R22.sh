#!/bin/bash
# generate_pdf_professor_v2.0.0-R22.sh
# Version: v2.0.2-R22 (Modified for progress reporting)
# MAJOR: Multi-mode operation (R17)

# 1. Configuration
BASE_DIR="$(dirname "$0")"
LISTA_DIR="$BASE_DIR/lista"
PROFESSORES_FILE="$LISTA_DIR/professores.txt"
CATEGORIES_FILE="$BASE_DIR/config/categories.txt" # NEW: Path to categories file
OUTPUT_DIR="$BASE_DIR/final"
TEMP_DIR_BASE="$BASE_DIR/pdf_temp" # Base temporary directory

# Set the maximum number of concurrent PDF generation jobs
MAX_CONCURRENT_JOBS=4 # Adjust this value based on your system's capabilities (e.g., number of CPU cores)

# Ensure necessary directories exist
mkdir -p "$LISTA_DIR" "$OUTPUT_DIR" "$TEMP_DIR_BASE" "$BASE_DIR/config" # Ensure base temp directory and config exist

# 2. Helpers (Moved to top for early availability)
log() {
    # If in single mode, output to stderr as stdout is used for PROGRESS
    if [ "$MODE" == "single" ]; then
        echo -e "[$(date +'%Y-%m-%d %H:%M:%S')] $1" >&2
    else
        echo -e "[$(date +'%Y-%m-%d %H:%M:%S')] $1"
    fi
}

# Verificar estrutura de diretórios
verify_structure() {
    local PROFESSOR_DIR_TO_CHECK="$BASE_DIR/$1" # Use a distinct variable name for clarity

    if [ ! -d "$PROFESSOR_DIR_TO_CHECK" ]; then
        echo "Erro: Diretório do professor '$PROFESSOR_DIR_TO_CHECK' não encontrado!"
        echo "Estrutura esperada:"
        echo "  $PROFESSOR_DIR_TO_CHECK/Comissões Examinadoras"
        echo "  $PROFESSOR_DIR_TO_CHECK/Produção Docente"
        # ... outras pastas
        exit 1
    fi
}

# 3. Mode detection
if [ $# -eq 0 ]; then
    echo -e "\033[31mError: No arguments provided\033[0m"
    echo "Usage:"
    echo "  Single mode: $0 'Professor Name'"
    echo "  Batch mode:  $0 all"
    exit 1
fi

if [ "$1" == "all" ]; then
    MODE="batch"
else
    MODE="single"
    PROFESSOR_NAME="$1" # This correctly captures the professor name for single mode
fi

# DEBUG: Print detected mode and argument
log "DEBUG: Argument received: '$1' (length: ${#1})"
log "DEBUG: Detected MODE: '$MODE'"
if [ "$MODE" == "single" ]; then
    log "DEBUG: Detected PROFESSOR_NAME: '$PROFESSOR_NAME' (length: ${#PROFESSOR_NAME})"
fi

# process_metadata function (appears to be unused, keeping for consistency with original file)
process_metadata() {
    local PROFESSOR_NAME="$1"
    local METADATA_FILE="metadata.json"

    # Extrair dados do JSON
    local PDF_COUNT=$(jq '.pdfs | length' "$METADATA_FILE")

    for ((i=0; i<PDF_COUNT; i++)); do
        local CATEGORY=$(jq -r ".pdfs[$i].category" "$METADATA_FILE")
        local TITLE=$(jq -r ".pdfs[$i].title" "$METADATA_FILE")
        local YEAR=$(jq -r ".pdfs[$i].year" "$METADATA_FILE")
        local SRC_PATH=$(jq -r ".pdfs[$i].path" "$METADATA_FILE")

        # Criar diretório da categoria se necessário
        local TARGET_DIR="${PROFESSOR_DIR}/${CATEGORY}"
        mkdir -p "$TARGET_DIR"

        # Gerar nome único para o PDF
        local FILENAME="${YEAR}_${TITLE// /_}.pdf"

        # Copiar PDF para a pasta do professor
        cp "$SRC_PATH" "${TARGET_DIR}/${FILENAME}"

        # Atualizar metadados
        exiftool -q -overwrite_original \
            -Title="$TITLE" \
            -Author="$PROFESSOR_NAME" \
            -Keywords="$CATEGORY" \
            "${TARGET_DIR}/${FILENAME}"
    done
}


generate_report() {

    local current_professor_name="$1" # Renamed for clarity within this function
    local PROFESSOR_DIR="$BASE_DIR/$current_professor_name"

    verify_structure "$current_professor_name" # Pass the actual professor name

    # NEW: Temporary directory for the current professor, keeping spaces
    local TEMP_DIR="$TEMP_DIR_BASE/$current_professor_name"
    local total_pdfs_processed_for_professor=0
    local PROCESSED_PDFS=0
    local PROFESSOR_ESCAPE
    local SECTION_NAME
    local caminho_pasta
    local doc_number
    local doc_num
    local SAFE_NAME
    local PAGES
    local TITLE
    local YEAR # Declare YEAR variable
    local -a pastas # Declare as array
    local -a pdfs


    local OUTPUT="${current_professor_name// /_}_relatorio_profissional.pdf" # Output PDF name still uses underscores for consistency with common file naming
    mkdir -p "$TEMP_DIR" # Create the temp directory with spaces

    # Verificar diretório do professor
    if [ ! -d "$PROFESSOR_DIR" ]; then
        echo "Erro: Diretório do professor '$PROFESSOR_DIR' não encontrado." >&2 # Output error to stderr
        echo "Nota: Execute o script do diretório pai contendo a pasta do professor" >&2
        exit 1
    fi

    # 2. Definir estrutura de pastas (Read from categories.txt)
    if [ ! -f "$CATEGORIES_FILE" ]; then
        log "\033[31mError: categories.txt not found in $BASE_DIR/config\033[0m"
        exit 1
    fi

    if [ ! -s "$CATEGORIES_FILE" ]; then
        log "\033[31mError: categories.txt is empty. Please add categories, one per line.\033[0m"
        exit 1
    fi

    pastas=() # Initialize empty array
    while IFS= read -r category || [ -n "$category" ]; do
        category=$(echo "$category" | tr -d '\r') # Remove carriage return
        if [ -z "$category" ]; then continue; fi # Skip empty lines
        pastas+=("$category")
    done < "$CATEGORIES_FILE"

    # Ordenar pastas alfabeticamente (case-insensitive)
    IFS=$'\n'
    local -a pastas=($(sort -f <<<"${pastas[*]}"))
    unset IFS

    # --- PROGRESS CALCULATION START ---
    local total_pdfs_expected_for_professor=0
    for pasta_count_pdfs in "${pastas[@]}"; do
        local caminho_pasta_count_pdfs="$PROFESSOR_DIR/$pasta_count_pdfs"
        if [ -d "$caminho_pasta_count_pdfs" ]; then
            total_pdfs_expected_for_professor=$((total_pdfs_expected_for_professor + $(find "$caminho_pasta_count_pdfs" -maxdepth 1 -name "*.pdf" | wc -l)))
        fi
    done

    # Initial progress report
    echo "PROGRESS: 0"
    # --- PROGRESS CALCULATION END ---

    # 3. Criar template LaTeX com ABNT
    cat > "$TEMP_DIR/metadata.tex" <<TEX
\documentclass[12pt, a44paper, oneside]{abntex2}
\usepackage[brazil]{babel}
\usepackage[utf8]{inputenc}
\usepackage{chngpage}
\usepackage{graphicx}
\usepackage{bookmark}
\usepackage{pdfpages}
\usepackage{fancyhdr}
\usepackage{lastpage}
\usepackage{times}
\usepackage{fmtcount}


\newcounter{doccounter}
\renewcommand{\thedoccounter}{\padzeroes[2]{\arabic{doccounter}}}


\hypersetup{
    colorlinks=true,
    linkcolor=blue,
    pdftitle={Relatório Profissional - $current_professor_name},
    pdfauthor={Universidade Federal de Pelotas},
    pdfsubject={Documentação Acadêmica},
    pdfkeywords={ABNT, UFPel, Relatório},
    pdfcreator={Script de Geração de PDF Acadêmico}
}

% Configuração de página
\setlength{\marginparwidth}{0pt}
\setlrmarginsandblock{3cm}{2cm}{*}
\setulmarginsandblock{3cm}{2cm}{*}
\checkandfixthelayout

% Capa ABNT - Ajustado para abntex2 (Removido ambiente titlepage)
\begin{document}
    \begin{center}
    %\vspace*{1cm}
    \includegraphics[width=0.3\textwidth]{../../logo_ufpel.png} % Altere para o caminho do logo

    \vspace{2cm}
    {\LARGE \textbf{UNIVERSIDADE FEDERAL DE PELOTAS}}

    \vspace{2cm}
    {\LARGE \textbf{APRESENTAÇÃO DA CARREIRA E PRODUÇÃO ACADÊMICA DE 2022 A 2024}}

    \vspace{4cm}
    {\Large Docente: \\ \textbf{$current_professor_name}}

    \vfill
    {\large \textbf{PELOTAS - RS}}\\
    {\large \textbf{\the\year}}
    \end{center}
\newpage % Ensures this content is on its own page
\pagestyle{fancy}
\fancyhf{}
\fancyhead[R]{\hyperlink{toc}{Voltar ao Sumário}}
\fancyfoot[C]{\thepage\ de \pageref{LastPage}}

\hypertarget{toc}{}
\pdfbookmark[0]{\contentsname}{toc}
\tableofcontents*
TEX

# 4. Processar pastas e PDFs
# Pré-processar nome do professor para LaTeX
local PROFESSOR_ESCAPE=$(echo "$current_professor_name" | sed -e 's/[&%$#_{}\\~^]/\\&/g' -e 's/\\\\/\\backslash/g')

for pasta in "${pastas[@]}"; do
    caminho_pasta="$PROFESSOR_DIR/$pasta"

    [ -d "$caminho_pasta" ] || continue

    # Obter PDFs ordenados numericamente
    declare -a pdfs
    mapfile -d '' pdfs < <(find "$caminho_pasta" -maxdepth 1 -name "*.pdf" -print0 | sort -V -z)

    [ ${#pdfs[@]} -gt 0 ] || continue

    # Process section name
    local SECTION_NAME=$(echo "$pasta" | sed -e 's/[&%$#_{}\\~^]/\\&/g')

    # Add section to LaTeX
    echo "\\clearpage" >> "$TEMP_DIR/metadata.tex"
    echo "\\section*{$SECTION_NAME}" >> "$TEMP_DIR/metadata.tex"
    echo "\\addcontentsline{toc}{section}{$SECTION_NAME}" >> "$TEMP_DIR/metadata.tex"
    echo "\\pdfbookmark[1]{$SECTION_NAME}{sec_${pasta//[^a-zA-Z]/_}}" >> "$TEMP_DIR/metadata.tex"

    # CRITICAL SECTION ADDITION
    local FIRST_DOC_ID=$((total_pdfs_processed_for_professor + 1))

    echo "\\vspace{1cm}" >> "$TEMP_DIR/metadata.tex"
    echo "\\begin{minipage}{\\textwidth}" >> "$TEMP_DIR/metadata.tex"
    echo "\\fontsize{12}{14}\\selectfont" >> "$TEMP_DIR/metadata.tex"


    local count=${#pdfs[@]}
    local plural_phrase="documentos"
    local article="os"
    local referentes="referentes"
    local verb="são apresentados"

    if [ "$count" -eq 1 ]; then
        plural_phrase="documento"
        article="o"
        verb="é apresentado"
        referentes="referente"
    fi

    local DATE_RANGE_CLAUSE=""
    if [[ "$SECTION_NAME" == "Produção Docente" ]]; then
        DATE_RANGE_CLAUSE=", cobrindo o período de 2022 a 2024"
    fi


    echo "\\noindent Nesta seção, $article \\textbf{$count} $plural_phrase $referentes a \\textbf{$SECTION_NAME} de \\textbf{$PROFESSOR_ESCAPE} $verb a seguir (\\hyperlink{startdoc$FIRST_DOC_ID}{\\color{blue}\\underline{clique aqui}})$DATE_RANGE_CLAUSE. Utilize o sistema de navegação no canto superior direito da página para retornar ao Sumário." >> "$TEMP_DIR/metadata.tex"
    echo "\\end{minipage}" >> "$TEMP_DIR/metadata.tex"
    echo "\\vspace{1.5cm}" >> "$TEMP_DIR/metadata.tex"
    # END CRITICAL SECTION ADDITION

    # Processar PDFs em ordem (loop modificado)
    doc_number=0

    for pdf in "${pdfs[@]}"; do
        ((total_pdfs_processed_for_professor++))
        ((doc_number++))
        printf -v doc_num "%02d" "$doc_number"

        # --- REVISED YEAR EXTRACTION LOGIC: PRIORITIZE METADATA DATES FROM ORIGINAL PDF ---
        local YEAR=""
        local METADATA_YEAR_FOUND="false"

        # Exiftool needs to operate on the ORIGINAL PDF for accurate metadata extraction
        # 1. Try DateTimeOriginal
        local DATETIMEORIGINAL_RAW=$(exiftool -DateTimeOriginal -s -s -s "$pdf" 2>/dev/null)
        if [ -n "$DATETIMEORIGINAL_RAW" ]; then
            if [[ "$DATETIMEORIGINAL_RAW" =~ ^([0-9]{4}) ]]; then # Extract first 4 digits (year)
                YEAR="${BASH_REMATCH[1]}"
                METADATA_YEAR_FOUND="true"
            fi
        fi

        # 2. If not found or invalid, try CreateDate
        if [ "$METADATA_YEAR_FOUND" = "false" ]; then
            local CREATEDATE_RAW=$(exiftool -CreateDate -s -s -s "$pdf" 2>/dev/null)
            if [ -n "$CREATEDATE_RAW" ]; then
                if [[ "$CREATEDATE_RAW" =~ ^([0-9]{4}) ]]; then
                    YEAR="${BASH_REMATCH[1]}"
                    METADATA_YEAR_FOUND="true"
                fi
            fi
        fi

        # 3. If still no year from date metadata, try ModifyDate
        if [ "$METADATA_YEAR_FOUND" = "false" ]; then
            local MODIFYDATE_RAW=$(exiftool -ModifyDate -s -s -s "$pdf" 2>/dev/null)
            if [ -n "$MODIFYDATE_RAW" ]; then
                if [[ "$MODIFYDATE_RAW" =~ ^([0-9]{4}) ]]; then
                    YEAR="${BASH_REMATCH[1]}"
                    METADATA_YEAR_FOUND="true"
                fi
            fi
        fi
        
        # 4. If still no year from date metadata, try the "Year" tag from original PDF
        if [ "$METADATA_YEAR_FOUND" = "false" ]; then
            local YEAR_TAG_RAW=$(exiftool -Year -s -s -s "$pdf" 2>/dev/null)
            if [ -n "$YEAR_TAG_RAW" ]; then
                if [[ "$YEAR_TAG_RAW" =~ ^([0-9]{4})$ ]]; then # Ensure it's exactly 4 digits
                    YEAR="${BASH_REMATCH[1]}"
                    METADATA_YEAR_FOUND="true"
                fi
            fi
        fi

        # 5. Fallback to filename if all metadata extraction failed
        if [ "$METADATA_YEAR_FOUND" = "false" ]; then
            local FILENAME_NO_EXT=$(basename "$pdf" .pdf)
            local EXTRACTED_YEAR_FROM_FILENAME=$(echo "$FILENAME_NO_EXT" | grep -oP '(\d{4})$' | tail -n 1)
            if [ -n "$EXTRACTED_YEAR_FROM_FILENAME" ]; then
                YEAR="$EXTRACTED_YEAR_FROM_FILENAME"
            fi
        fi

        # Final fallback if no year could be determined
        if [ -z "$YEAR" ]; then
            YEAR="Ano Desconhecido"
        fi
        # --- END REVISED YEAR EXTRACTION LOGIC ---

        local SAFE_NAME="doc_${total_pdfs_processed_for_professor}.pdf"

        # Conversão original da v1.1 (sem injeção de título)
        # This gs command will likely set its own CreationDate/ModDate metadata,
        # but we have already extracted the YEAR from the original PDF.
        gs -q -dNOPAUSE -dBATCH -sDEVICE=pdfwrite \
           -dCompatibilityLevel=1.7 \
           -dPDFSETTINGS=/prepress \
           -sOutputFile="$TEMP_DIR/$SAFE_NAME" \
           "$pdf"

        # Extract TITLE from metadata (retained original logic, operating on SAFE_NAME now)
        # Note: Title extraction is still from the (potentially re-generated) SAFE_NAME.
        # If original title is needed consistently, this would also need to happen on '$pdf' before gs.
        local TITLE=$(exiftool -Title -s -s -s "$TEMP_DIR/$SAFE_NAME" 2>/dev/null || basename "$pdf" .pdf)
        TITLE=$(echo "$TITLE" | sed -e 's/[&%$#_{}\\~^]/\\&/g')

        # Now, explicitly set the YEAR tag in the new SAFE_NAME PDF using the extracted YEAR
        exiftool -q -overwrite_original -Year="$YEAR" "$TEMP_DIR/$SAFE_NAME"

        log "Processing PDF: $pdf"
        log "Extracted Title: \"$TITLE\""
        log "Extracted Year: \"$YEAR\" (from original metadata or filename)"

        # Conditional addition of YEAR to subsection and TOC based on SECTION_NAME
        if [[ "$SECTION_NAME" == "Produção Docente" ]]; then
            echo "\\clearpage" >> "$TEMP_DIR/metadata.tex"
            echo "\\subsection*{$doc_num - $TITLE ($YEAR)}" >> "$TEMP_DIR/metadata.tex"
            echo "\\addcontentsline{toc}{subsection}{$doc_num - $TITLE ($YEAR)}" >> "$TEMP_DIR/metadata.tex"
            echo "\\pdfbookmark[2]{$doc_num - $TITLE ($YEAR)}{doc$total_pdfs_processed_for_professor}" >> "$TEMP_DIR/metadata.tex"
            # Text for the current subsection, including year
            echo "\\vspace{1cm}" >> "$TEMP_DIR/metadata.tex"
            echo "\\begin{minipage}{\\textwidth}" >> "$TEMP_DIR/metadata.tex"
            echo "\\fontsize{12}{14}\\selectfont" >> "$TEMP_DIR/metadata.tex"
            echo "\\noindent Nesta subseção, a leitura do documento \\textbf{$doc_num - $TITLE ($YEAR)} pode ser realizada na próxima página (\\hyperlink{startdoc$total_pdfs_processed_for_professor}{\\color{blue}\\underline{clique aqui}}). Utilize o sistema de navegação no canto superior direito da página para retornar ao Sumário." >> "$TEMP_DIR/metadata.tex"
            echo "\\end{minipage}" >> "$TEMP_DIR/metadata.tex"
            echo "\\vspace{1.5cm}" >> "$TEMP_DIR/metadata.tex"
        else
            # Adição ao documento (without YEAR)
            echo "\\clearpage" >> "$TEMP_DIR/metadata.tex"
            echo "\\subsection*{$doc_num - $TITLE}" >> "$TEMP_DIR/metadata.tex"
            echo "\\addcontentsline{toc}{subsection}{$doc_num - $TITLE}" >> "$TEMP_DIR/metadata.tex"
            echo "\\pdfbookmark[2]{$doc_num - $TITLE}{doc$total_pdfs_processed_for_professor}" >> "$TEMP_DIR/metadata.tex"
            # Text for the current subsection, without year
            echo "\\vspace{1cm}" >> "$TEMP_DIR/metadata.tex"
            echo "\\begin{minipage}{\\textwidth}" >> "$TEMP_DIR/metadata.tex"
            echo "\\fontsize{12}{14}\\selectfont" >> "$TEMP_DIR/metadata.tex"
            echo "\\noindent Nesta subseção, a leitura do documento \\textbf{$doc_num - $TITLE} pode ser realizada na próxima página (\\hyperlink{startdoc$total_pdfs_processed_for_professor}{\\color{blue}\\underline{clique aqui}}). Utilize o sistema de navegação no canto superior direito da página para retornar ao Sumário." >> "$TEMP_DIR/metadata.tex"
            echo "\\end{minipage}" >> "$TEMP_DIR/metadata.tex"
            echo "\\vspace{1.5cm}" >> "$TEMP_DIR/metadata.tex"
        fi

    # Add this RIGHT BEFORE including the PDF
    echo "\\newpage" >> "$TEMP_DIR/metadata.tex"
    echo "\\hypertarget{startdoc$total_pdfs_processed_for_professor}{}" >> "$TEMP_DIR/metadata.tex"

    # Then include the PDF
    echo "\\includepdf[pages=-,pagecommand={\\thispagestyle{fancy}}]{$SAFE_NAME}" >> "$TEMP_DIR/metadata.tex"

        log "Processado: $pdf → $SAFE_NAME (Número: $doc_num)"

        # --- PROGRESS REPORTING DURING PDF PROCESSING ---
        if [ "$total_pdfs_expected_for_professor" -gt 0 ]; then
            local current_percentage=$((total_pdfs_processed_for_professor * 90 / total_pdfs_expected_for_professor))
            echo "PROGRESS: $current_percentage"
        fi
        # --- END PROGRESS REPORTING ---
    done
done


# 5. Finalizar documento
echo "\\end{document}" >> "$TEMP_DIR/metadata.tex"

# --- PROGRESS REPORTING BEFORE COMPILATION ---
echo "PROGRESS: 95"
# --- END PROGRESS REPORTING ---

# 6. Compilar
log "Compiling PDF professional..."
(
    cd "$TEMP_DIR" || exit 1
    pdflatex -interaction=nonstopmode metadata.tex > compile.log
    pdflatex -interaction=nonstopmode metadata.tex >> compile.log
)

# 7. Finalizar
if [ -f "$TEMP_DIR/metadata.pdf" ]; then
    mkdir -p "final"
    gs -q -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.7 \
       -dPDFSETTINGS=/prepress -sOutputFile="final/$OUTPUT" "$TEMP_DIR/metadata.pdf"

    log "SUCESSO: PDF ABNT gerado com capa institucional"
    log "Arquivo final: final/$OUTPUT"
    echo "PROGRESS: 100"

    # Safely remove the temporary directory and its contents
    log "Cleaning up temporary directory: $TEMP_DIR"
    rm -rf "$TEMP_DIR"
    if [ $? -eq 0 ]; then
        log "Temporary directory removed successfully."
    else
        log "\033[31mWARNING: Failed to remove temporary directory '$TEMP_DIR'. Manual cleanup may be required.\033[0m"
    fi

else
    log "ERRO: Falha na geração do PDF"
    echo "PROGRESS: -1"
    exit 1
fi

}
# 4. Main execution
case "$MODE" in
    "batch") # Changed from "all" to "batch"
        log "Starting batch processing mode with up to $MAX_CONCURRENT_JOBS concurrent jobs"

        if [ ! -f "$PROFESSORES_FILE" ]; then
            log "\033[31mError: professores.txt not found in $LISTA_DIR\033[0m"
            exit 1
        fi

        if [ ! -s "$PROFESSORES_FILE" ]; then
            log "\033[31mError: professores.txt is empty\033[0m"
            exit 1
        fi

        local active_jobs=0
        local pids=()

        while IFS= read -r professor || [ -n "$professor" ]; do
            professor=$(echo "$professor" | tr -d '\r')
            if [ -z "$professor" ]; then continue; fi

            log "Scheduling: $professor"

            if (( active_jobs >= MAX_CONCURRENT_JOBS )); then
                wait -n
                ((active_jobs--))
            fi

            generate_report "$professor" &
            pids+=($!)
            ((active_jobs++))

        done < "$PROFESSORES_FILE"

        log "Waiting for all background jobs to complete..."
        for pid in "${pids[@]}"; do
            wait "$pid"
        done

        log "Batch processing completed"
        ;;

    "single") # Explicitly define "single" mode
        log "Starting single mode processing: $PROFESSOR_NAME"
        generate_report "$PROFESSOR_NAME"
        log "Single processing completed"
        ;;
    *) # Fallback for any other argument, though already handled by initial check
        echo -e "\033[31mError: Invalid mode or argument provided.\033[0m"
        echo "Usage:"
        echo "  Single mode: $0 'Professor Name'"
        echo "  Batch mode:  $0 all"
        exit 1
        ;;
esac

