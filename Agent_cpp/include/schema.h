#pragma once
#include <string>
#include <vector>

/* Schema definitions for multi-table database support.

This module provides type-safe dataclass models for describing a database's
tables and columns so the LLM can generate accurate multi-table SQL queries.
 */

namespace agent_cpp{

struct ColumnSchema{

    /* Description of a single column within a table.

    Attributes:
    name: Column name as it appears in the data file.
    description: Human-readable description for the LLM.
    data_type: SQL data type string (e.g., "VARCHAR", "FLOAT", "DATE", "INTEGER").
    example_values: Optional list of sample values to help the LLM understand the data.
    nullable: Whether the column can contain NULL values. */

    std::string name;
    std::string description;
    std::string data_type;
    std::vector<std::string> example_values;
    bool nullable = true;
};

struct TableSchema{

/*     Description of a single table backed by a parquet file.

    Attributes:
        name: Table name to use in DuckDB (must be a valid SQL identifier).
        description: Human-readable description of what this table contains.
        file_path: Absolute or relative path to the parquet file.
        columns: Ordered list of column definitions. */

    std::string name;
    std::string description;
    std::string file_path;
    std::vector<ColumnSchema> columns;
};

class DatabaseSchema{

/*  Container for all tables in the database, with LLM context helpers.

    Attributes:
        tables: All registered TableSchema objects.
 */

public:
    std::vector<TableSchema> tables;
    std::string get_full_schema_str() const;
    static DatabaseSchema from_data_dir(const std::string& data_dir);
};

} 