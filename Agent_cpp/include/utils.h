#pragma once

#include <string>
#include <vector>
#include <memory>
#include <duckdb.h>
#include <nlohmann/json.hpp>
#include <agents-cpp/context.h>
#include "schema.h"

namespace agent_cpp {

using JsonObject = nlohmann::json;

/**
 * @brief Sostituisce tutte le occorrenze di una sottostringa all'interno di una stringa.
 */
void replace_all(std::string& str, const std::string& from, const std::string& to);

/**
 * @brief Converte un risultato DuckDB in formato stringa CSV e lo salva in `Output/result.csv`.
 */
std::string query_result_to_csv(duckdb_result& result);

/**
 * @brief Inizializza il database DuckDB caricando in memoria le tabelle dai file Parquet definiti nello schema.
 */
bool init_database(duckdb_connection con, const DatabaseSchema& schema);

/**
 * @brief Calcola la similarità normalizzata in [0.0, 1.0] tra due stringhe tramite la distanza di Levenshtein (difflib SequenceMatcher).
 */
double sequence_similarity(const std::string& s1, const std::string& s2);

/**
 * @brief Struttura tabellare per il parsing del CSV per il confronto row-IoU.
 */
struct CsvTable {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

/**
 * @brief Esegue il parsing di una stringa CSV in CsvTable.
 */
CsvTable parse_csv_string(const std::string& csv_str);

/**
 * @brief Calcola l'Intersection-over-Union (IoU) a livello di riga tra due tabelle CSV.
 * Fedele a compare_dataframes_iou in Python (Agent/utils.py).
 */
double compare_csv_tables_iou(const CsvTable& t1, const CsvTable& t2, double atol = 0.05);

/**
 * @brief Valutatore di consenso per lookup_sales_data: calcola per ogni candidato
 * l'IoU medio a coppie rispetto a tutti gli altri candidati.
 * Fedele a make_csv_evaluator_no_gt() in Python (Agent/utils.py).
 */
std::vector<double> evaluate_csv_consensus(const std::vector<std::string>& csv_list);

/**
 * @brief Valutatore LLM-as-a-Judge per analyzing_data (correttezza, completezza, fedeltà).
 * Fedele a make_text_evaluator_no_gt() / judge_analysis() in Python (Agent/utils.py).
 */
double judge_analysis_text(
    const std::string& prompt,
    const std::string& sql_query,
    const std::string& data,
    const std::string& analysis,
    std::shared_ptr<agents::Context> context
);

/**
 * @brief Valutatore LLM-as-a-Judge per create_visualization.
 * Fedele a make_vis_evaluator_no_gt() / judge_visualization_no_gt() in Python (Agent/utils.py).
 */
double judge_visualization(
    const std::string& visualization_goal,
    const JsonObject& chart_config,
    const std::string& chart_code,
    const std::string& data_sample,
    std::shared_ptr<agents::Context> context
);

} // namespace agent_cpp
