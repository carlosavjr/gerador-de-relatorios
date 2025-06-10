Bem-vindo ao Gerador de Relatórios!
        Este programa foi projetado para organização de documentos acadêmicos e geração de relatório consolidado para cada professor.
        
        Fluxo de Trabalho:
        
        1.  Seleção do Professor:
            - No topo da janela, utilize o campo 'Selecione o Professor' para escolher o professor para o qual deseja gerar o relatório.\n
            - Se o professor não estiver na lista, clique em 'Configurações' para adicioná-lo à 'Lista de Professores'.\n\n
        2.  Organização dos Documentos (PDFs):
            - Ao selecionar um professor, o programa verifica automaticamente as pastas existentes para ele (ex: `./[Nome do Professor]/[Nome da Categoria]/`).
            - PDFs já presentes nessas pastas serão carregados automaticamente.
            - Para adicionar novos PDFs, clique em 'Adicionar PDF' na seção da categoria desejada.
            - Edição de Metadados: Para cada PDF, você pode editar o 'Título do Documento', o 'Ano' e a 'Categoria'. Todos devem estar presentes para as modificações serem salvas. Estes campos são cruciais para a organização e serão incluídos no relatório final. O programa tenta preencher esses campos a partir dos metadados do PDF ou do nome do arquivo.
            - Campo 'Num.': Este é o número que será usado no início do nome do arquivo (ex: '01_'). Você pode inseri-lo manualmente. Se deixado vazio, o programa tentará usar um número já presente no nome original do arquivo ou atribuirá o próximo número disponível automaticamente.
            - Botão 'Remover': Move o PDF para uma subpasta 'old_files' dentro da categoria, indicando que ele não será incluído no relatório atual.
            - Botão 'Visualizar': Abre o PDF selecionado em seu visualizador de PDF padrão.
        3.  Botões de Ação Importantes:
            - Recarregar Professor: Este botão é útil se você fez alterações nos arquivos ou na estrutura de pastas de um professor fora do aplicativo. Clicar nele recarregará os dados do professor selecionado, garantindo que o que você vê na interface esteja atualizado com o sistema de arquivos.
            - Salvar Alterações: Use este botão para salvar todas as modificações feitas nos campos de título, ano e número dos PDFs na interface. As alterações serão aplicadas aos arquivos PDF correspondentes, mas o relatório não será gerado automaticamente. Útil para salvar o progresso sem iniciar uma nova compilação de relatório.
            - Visualizar Relatório Profissional: Este botão permite visualizar o relatório em PDF que foi gerado anteriormente para o professor selecionado. Se o relatório ainda não foi gerado, é preciso clicar em 'Gerar Relatório Profissional' primeiro.\n\n
        4.  Geração do Relatório:
            - Após organizar todos os PDFs e seus metadados, clique em 'Gerar Relatório Profissional'.\n
            - O programa copiará os PDFs para a estrutura de pastas correta do professor, aplicará os metadados editados e executará um script de backend para compilar o relatório em formato PDF.
            - Ao finalizar, o relatório será aberto automaticamente.
            - Um botão 'Visualizar Relatório Profissional' aparecerá para acesso rápido ao relatório gerado.
        5.  Configurações:
            - O botão 'Configurações' permite gerenciar a 'Lista de Professores' e a 'Lista de Categorias'. Altere-os conforme a necessidade e salve as alterações para que sejam aplicadas.
        Estrutura de Pastas Esperada:
        O programa cria e espera uma estrutura de pastas semelhante a:
        ├── config/
        │   ├── categories.txt
        │   └── professores.txt
        ├── final/
        ├── pdf_temp/
        └── [Nome do Professor]/
            ├── [Nome da Categoria 1]/
            │   └── seus_documentos.pdf
            │   └── old_files/
            └── [Nome da Categoria 2]/
                └── outro_documento.pdf
                └── old_files/
        
