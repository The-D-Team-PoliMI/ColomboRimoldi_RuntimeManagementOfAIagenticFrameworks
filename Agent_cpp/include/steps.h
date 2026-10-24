#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <duckdb.h>
#include <agents-cpp/context.h>
#include "schema.h"

namespace agent_cpp{

using JsonObject = nlohmann::json;

/**
 * @brief Estrae la stringa di output principale di uno step per il calcolo della similarità nel CoT.
 * @param step_name Nome dello step ("lookup_sales_data", "decide_tool", ecc.).
 * @param result Stato JSON restituito dallo step.
 * @return std::string Query SQL, nome del tool o ultimo elemento dell'array `answer`.
 */
std::string extract_step_output(const std::string& step_name, const JsonObject& result);

/**
 * @brief Arricchisce il prompt di base con il blocco di Iterative Refinement se siamo in una iterazione CoT > 1.
 * @param base_prompt Prompt standard dello step.
 * @param state Stato contenente `_cot_previous_response` ed eventualmente `_cot_execution_error`.
 * @return std::string Prompt completo pronto per l'invio all'LLM.
 */
std::string format_cot_prompt(const std::string& base_prompt, const JsonObject& state);

/**
 * @brief Nodo orchestratore: determina il prossimo tool da eseguire in base allo stato attuale.
 * @param state Stato corrente della conversazione.
 * @param context Contesto con il client LLM.
 * @param schema Schema del database.
 * @return JsonObject Stato aggiornato con il campo `tool_choice`.
 */
JsonObject DecideTool(JsonObject& state, std::shared_ptr<agents::Context> context);

/**
 * @brief Nodo di data lookup: genera una query SQL tramite LLM e la esegue su DuckDB.
 * @param state Stato corrente contenente `prompt` e `visualization_goal`.
 * @param context Contesto con il client LLM.
 * @param schema Schema del database per popolare il prompt SQL.
 * @param con Connessione DuckDB su cui eseguire la query generata.
 * @return JsonObject Stato aggiornato con `sql_query`, `data` (CSV) o `error`.
 */
JsonObject LookupSalesData(JsonObject& state, std::shared_ptr<agents::Context> context, const DatabaseSchema& schema, duckdb_connection con);

/**
 * @brief Nodo di analisi: genera un'analisi testuale basata sui dati estratti e sulle statistiche pre-calcolate.
 * @param state Stato contenente i dati CSV (`data`) e la query (`sql_query`).
 * @param context Contesto con il client LLM.
 * @param schema Schema del database.
 * @return JsonObject Stato con l'analisi aggiunta alla lista `answer`.
 */
JsonObject AnalyzingData(JsonObject& state, std::shared_ptr<agents::Context> context);

/**
 * @brief Nodo di visualizzazione: estrae la configurazione JSON del grafico, genera il codice Matplotlib e lo valida.
 * @param state Stato contenente i dati CSV e il `visualization_goal`.
 * @param context Contesto con il client LLM.
 * @param schema Schema del database.
 * @return JsonObject Stato aggiornato con `chart_config`, codice Python in `answer` ed eventuale errore di validazione.
 */
JsonObject CreateVisualization(JsonObject& state, std::shared_ptr<agents::Context> context);

} 