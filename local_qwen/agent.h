#pragma once

#include <string>

namespace local_qwen {

class Agent {
public:
    int run();

private:
    std::string history_;
};

} // namespace local_qwen
