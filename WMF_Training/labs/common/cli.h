// cli.h — 極簡命令列參數解析：--name value 或 --flag
#pragma once

#include <cstdlib>
#include <string>
#include <vector>

class Args {
public:
    Args(int argc, char** argv) : args_(argv + 1, argv + argc) {}

    bool Has(const std::string& name) const
    {
        for (const auto& a : args_)
            if (a == name) return true;
        return false;
    }
    std::string Get(const std::string& name, const std::string& def) const
    {
        for (size_t i = 0; i + 1 < args_.size(); ++i)
            if (args_[i] == name) return args_[i + 1];
        return def;
    }
    long GetInt(const std::string& name, long def) const
    {
        std::string v = Get(name, "");
        return v.empty() ? def : std::strtol(v.c_str(), nullptr, 10);
    }
    // 第一個不是 --xxx 也不是某個 --xxx 參數值的字串（例如輸入檔名）
    std::string Positional(size_t index, const std::string& def) const
    {
        size_t n = 0;
        for (size_t i = 0; i < args_.size(); ++i) {
            if (args_[i].rfind("--", 0) == 0) {
                if (i + 1 < args_.size() && args_[i + 1].rfind("--", 0) != 0 && !IsFlag(args_[i])) ++i;
                continue;
            }
            if (n++ == index) return args_[i];
        }
        return def;
    }
    void DeclareFlags(std::vector<std::string> flags) { flags_ = std::move(flags); }

private:
    bool IsFlag(const std::string& a) const
    {
        for (const auto& f : flags_)
            if (f == a) return true;
        return false;
    }
    std::vector<std::string> args_;
    std::vector<std::string> flags_;
};
