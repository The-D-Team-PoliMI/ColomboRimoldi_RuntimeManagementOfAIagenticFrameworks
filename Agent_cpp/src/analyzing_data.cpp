#include "analyzing_data.h"
#include "utils.h"
#include <iostream>

namespace agent_cpp{

const std::string AnalyzingData::DATA_ANALYSIS_PROMPT = R"(You are a professional data analyst providing insights from query results.

## TASK
Answer the user's question based ONLY on the provided data.

## USER QUESTION
{prompt}

## AVAILABLE DATA
This data was retrieved using the SQL query: {sql_query}

Data:
{data}

## INSTRUCTIONS
1. Examine the data carefully to understand what information is available
2. Identify the key insights that directly answer the user's question
3. Provide a specific answer: aim for 2-3 sentences, but use up to 6 if needed to cover all key facts
4. Use actual numbers and facts from the data
5. Do NOT speculate or make assumptions beyond what the data shows
6. If the data doesn't fully answer the question, state what you can determine from the available data

## CHAIN OF THOUGHT REASONING
Before answering, think step by step:

**Step 1: Understanding the Question**
- What specific information is the user asking for?
- Is it asking for a single value, a comparison, a trend, or a summary?
- What would constitute a complete answer?

**Step 2: Examining the Data Structure**
- How many rows of data are available?
- What columns are present in the data?
- What is the range or distribution of values?
- Are there any patterns or anomalies visible?

**Step 3: Extracting Relevant Facts**
- Which specific values directly answer the question?
- Do I need to perform mental calculations (sum, average, count)?
- What are the exact numbers, dates, or categories relevant to the answer?
- Are there any context clues (time periods, units, categories)?

**Step 4: Verifying Completeness**
- Does the data fully answer the user's question?
- Is there missing information that prevents a complete answer?
- Should I mention any limitations or caveats?

**Step 5: Formulating the Answer**
- How can I state the facts concisely (2-3 sentences)?
- Am I using specific numbers from the data?
- Am I avoiding speculation or assumptions?
- Is my answer direct and clear?

## OUTPUT FORMAT
Provide a direct, concise answer in natural language (2-3 sentences). Focus only on facts from the data.
)";

AnalyzingData::AnalyzingData(std::shared_ptr<agents::Context> ctx): context_(ctx) {}

// Esegue l'analisi qualitativa dei dati ottenuti da lookup_sales_data
JsonObject AnalyzingData::run(JsonObject& state){
    if(state.contains("error") && !state["error"].get<std::string>().empty()) 
        return state;

    std::string prompt = state.value("prompt", "");
    std::string sql_query = state.value("sql_query", "");
    std::string data = state.value("data", "");

    std::string formatted_prompt = DATA_ANALYSIS_PROMPT;
    replace_all(formatted_prompt, "{prompt}", prompt);
    replace_all(formatted_prompt, "{sql_query}", sql_query);
    replace_all(formatted_prompt, "{data}", data);

    try{
        auto llm = context_->getLLM();
        auto response = llm->chat(formatted_prompt);
        std::string analysis_text = response.content;

        std::cout << "Analysis:\n" << analysis_text << "\n" << std::endl;

        if(!state.contains("answer") || !state["answer"].is_array()){
            state["answer"] = nlohmann::json::array();
        }
        state["answer"].push_back(analysis_text);

    } catch (const std::exception& e) {
        state["error"] = std::string("Error analyzing data: ") + e.what();
        std::cerr << "[AnalyzingData] Error: " << e.what() << std::endl;
    }

    return state;
}

} 