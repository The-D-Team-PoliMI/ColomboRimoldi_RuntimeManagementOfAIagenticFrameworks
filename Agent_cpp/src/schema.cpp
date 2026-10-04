#include "schema.h"
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

namespace agent_cpp{

std::string DatabaseSchema::get_full_schema_str() const{
/*     Return full schema details including column descriptions.

        Returns a string like::
            Table: sales
            Description: Daily store-level sales transactions
            Columns:
              - Sold_Date (DATE): Transaction date
              - Total_Sale_Value (FLOAT): Total revenue in USD [examples: 100.0, 250.5]
              - Store_Number (INTEGER): Unique store identifier [NOT NULL]
               */

    std::stringstream ss;
    for (const auto& table : tables) {
        ss << "Table: " << table.name << "\n";
        ss << "Description: " << table.description << "\n";
        if (!table.columns.empty()) {
            ss << "Columns:\n";
            for (const auto& col : table.columns) {
                ss << "  - " << col.name << " (" << col.data_type << "): " << col.description;
                if (!col.nullable) ss << " [NOT NULL]";
                if (!col.example_values.empty()) {
                    ss << " [examples: ";
                    for (size_t i = 0; i < col.example_values.size(); ++i) {
                        ss << col.example_values[i] << (i < col.example_values.size() - 1 ? ", " : "");
                    }
                    ss << "]";
                }
                ss << "\n";
            }
        }
        ss << "\n";
    }
    return ss.str();
}

// costruisce in memoria l'oggetto DatabaseSchema
DatabaseSchema DatabaseSchema::from_data_dir(const std::string& data_dir) {
    DatabaseSchema schema;
    std::vector<fs::path> schema_files;

    if (!fs::exists(data_dir) || !fs::is_directory(data_dir)) {
        throw std::runtime_error("Directory non valida: " + data_dir);
    }

    // Cerca file con pattern *_schema.yaml
    for (const auto& entry : fs::directory_iterator(data_dir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.ends_with("_schema.yaml")) {
                schema_files.push_back(entry.path());
            }
        }
    }
    std::sort(schema_files.begin(), schema_files.end());

    for (const auto& path : schema_files) {
        YAML::Node root = YAML::LoadFile(path.string());
        TableSchema table;
        table.name = root["name"].as<std::string>();
        table.description = root["description"] ? root["description"].as<std::string>() : table.name;

        std::string raw_file_path = root["file_path"].as<std::string>();
        fs::path parquet_path = raw_file_path;
        if (!parquet_path.is_absolute()) {
            parquet_path = path.parent_path() / parquet_path;
        }
        table.file_path = fs::absolute(parquet_path).string();

        if (root["columns"]) {
            for (const auto& col_node : root["columns"]) {
                ColumnSchema col;
                col.name = col_node["name"].as<std::string>();
                col.description = col_node["description"] ? col_node["description"].as<std::string>() : col.name;
                col.data_type = col_node["data_type"] ? col_node["data_type"].as<std::string>() : "VARCHAR";
                col.nullable = col_node["nullable"] ? col_node["nullable"].as<bool>() : true;

                if (col_node["example_values"]) {
                    for (const auto& ex : col_node["example_values"]) {
                        col.example_values.push_back(ex.as<std::string>());
                    }
                }
                table.columns.push_back(col);
            }
        }
        schema.tables.push_back(table);
    }

    return schema;
}

} 