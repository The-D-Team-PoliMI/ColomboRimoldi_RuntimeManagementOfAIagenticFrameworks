#include "utils.h"

namespace agent_cpp{

    //Sostituisce tutte le occorrenze della sottostringa from con la stringa to all'interno della stringa str
    void replace_all(std::string& str, const std::string& from, const std::string& to){
        if(from.empty()) return;
        size_t start_pos = 0;
        while((start_pos = str.find(from, start_pos)) != std::string::npos){
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    }
}