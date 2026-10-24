#pragma once
#include <string>
#include <vector>

namespace agent_cpp{

 /**
 * @brief Descrizione di una singola colonna di una tabella del database.
 */
struct ColumnSchema{
    std::string name; 
    std::string description;
    std::string data_type;
    std::vector<std::string> example_values;
    bool nullable = true;
};

 /**
 * @brief Descrizione di una tabella del database.
 */
struct TableSchema{
    std::string name;
    std::string description;
    std::string file_path;
    std::vector<ColumnSchema> columns;
};

/**
 * @brief Container dello schema completo del database con helper di formattazione per i prompt LLM.
 */
class DatabaseSchema{
    public:
        std::vector<TableSchema> tables;

        /**
         * @brief Genera una stringa formattata con i dettagli completi di tabelle e colonne da iniettare nel prompt.
         * @return std::string Rappresentazione testuale dello schema.
         */
        std::string get_full_schema_str() const;

        /**
         * @brief Costruisce l'oggetto DatabaseSchema scansionando tutti i file `*_schema.yaml` in una directory.
         * @param data_dir Percorso della directory contenente gli schemi YAML e i file Parquet.
         * @return DatabaseSchema Istanza popolata con tutte le tabelle trovate.
         * @throws std::runtime_error Se la directory specificata non esiste o non è valida.
         */
        static DatabaseSchema from_data_dir(const std::string& data_dir);
};

} 