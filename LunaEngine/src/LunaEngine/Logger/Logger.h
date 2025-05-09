#pragma once
#include <iostream>

#define LUNA_LOG_INFO(x) std::cout << "[INFO] " << x << std::endl;
#define LUNA_LOG_WARN(x) std::cout << "[WARN] " << x << std::endl;
#define LUNA_LOG_ERROR(x) std::cout << "[ERROR] " << x << std::endl;