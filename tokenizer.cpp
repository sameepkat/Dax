#include <cctype>
#include <iostream>
#include <vector>
#include <sstream>

std::vector<std::string> tokenize(const std::string &line) {
  enum State { NORMAL , IN_SINGLE , IN_DOUBLE } state = NORMAL;
  std::vector<std::string> tokens;
  std::string token, buf;

  tokens.clear();
  std::istringstream stream(line);
  // while(stream >> token) tokens.push_back(token);


  for (int i=0; i<line.size(); ++i) {
      //std::cout << line[i] << "|";

    switch (state) {
    case NORMAL:
        if(line[i]=='\''){state = IN_SINGLE;}
        else if(line[i] == '"') { state = IN_DOUBLE;
        } else if (std::isspace(line[i])) {
            if(!buf.empty()){tokens.push_back(buf); buf.clear();}
        } else {
            buf += line[i];
        }
        break;

    case IN_SINGLE:
      if (line[i] == '\'')
        state = NORMAL;
      else
        buf += line[i];
      break;

    case IN_DOUBLE:
        if (line[i] == '"')
          state = NORMAL;
        else
            buf += line[i];
      break;
    }
  } 

  if(!buf.empty()) tokens.push_back(buf);
  return tokens;
}
