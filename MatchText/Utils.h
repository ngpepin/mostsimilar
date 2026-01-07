#pragma once

#include <string>

class Statistics;

std::string ReadAllText(const std::string& filePath);
bool ReadFileToStatistics(const std::string& filePath, Statistics* stats);
bool ReadFileToStatistics(const std::string& filePath,
                          Statistics* stats,
                          bool safe_mode,
                          bool no_convert);
bool IsAllowedTextFile(const std::string& filePath);
