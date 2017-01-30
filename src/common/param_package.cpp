// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <vector>
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/string_util.h"

namespace Common {

constexpr char KEY_VALUE_SEPARATER = ':';
constexpr char PARAM_SEPARATER = ',';
constexpr char ESCAPE_CHARACTER = '$';
const std::string KEY_VALUE_SEPARATER_ESCAPE{ESCAPE_CHARACTER, '0'};
const std::string PARAM_SEPARATER_ESCAPE{ESCAPE_CHARACTER, '1'};
const std::string ESCAPE_CHARACTER_ESCAPE{ESCAPE_CHARACTER, '2'};

ParamPackage::ParamPackage(const std::string& serialized) {
    std::vector<std::string> pairs;
    Common::SplitString(serialized, PARAM_SEPARATER, pairs);

    for (const std::string& pair : pairs) {
        std::vector<std::string> key_value;
        Common::SplitString(pair, KEY_VALUE_SEPARATER, key_value);
        if (key_value.size() != 2) {
            LOG_ERROR(Common, "invalid key pair %s", pair.c_str());
            continue;
        }

        for (std::string& part : key_value) {
            part = Common::ReplaceAll(part, KEY_VALUE_SEPARATER_ESCAPE, {KEY_VALUE_SEPARATER});
            part = Common::ReplaceAll(part, PARAM_SEPARATER_ESCAPE, {PARAM_SEPARATER});
            part = Common::ReplaceAll(part, ESCAPE_CHARACTER_ESCAPE, {ESCAPE_CHARACTER});
        }

        Set(key_value[0], key_value[1]);
    }
}

ParamPackage::ParamPackage(std::initializer_list<DataType::value_type> list) : data(list) {}

ParamPackage::ParamPackage(ParamPackage&& other) : data(std::move(other.data)) {}

ParamPackage& ParamPackage::operator=(ParamPackage&& other) {
    data = std::move(other.data);
    return *this;
}

std::string ParamPackage::Serialize() const {
    std::string result;
    if (data.empty())
        return "";

    for (auto pair : data) {
        std::string key_value[2]{pair.first, pair.second};
        for (std::string& part : key_value) {
            part = Common::ReplaceAll(part, {ESCAPE_CHARACTER}, ESCAPE_CHARACTER_ESCAPE);
            part = Common::ReplaceAll(part, {PARAM_SEPARATER}, PARAM_SEPARATER_ESCAPE);
            part = Common::ReplaceAll(part, {KEY_VALUE_SEPARATER}, KEY_VALUE_SEPARATER_ESCAPE);
        }
        result += key_value[0] + KEY_VALUE_SEPARATER + key_value[1] + PARAM_SEPARATER;
    }

    result.resize(result.size() - 1); // discard the trailing PARAM_SEPARATER
    return result;
}

std::string ParamPackage::Get(const std::string& key, const std::string& default_value) const {
    auto pair = data.find(key);
    if (pair == data.end()) {
        LOG_DEBUG(Common, "key %s not found", key.c_str());
        return default_value;
    } else {
        return pair->second;
    }
}

int ParamPackage::Get(const std::string& key, int default_value) const {
    auto pair = data.find(key);
    if (pair == data.end()) {
        LOG_DEBUG(Common, "key %s not found", key.c_str());
        return default_value;
    } else {
        try {
            return std::stoi(pair->second);
        } catch (...) {
            LOG_ERROR(Common, "failed to convert %s to int", pair->second.c_str());
            return default_value;
        }
    }
}

float ParamPackage::Get(const std::string& key, float default_value) const {
    auto pair = data.find(key);
    if (pair == data.end()) {
        LOG_DEBUG(Common, "key %s not found", key.c_str());
        return default_value;
    } else {
        try {
            return std::stof(pair->second);
        } catch (...) {
            LOG_ERROR(Common, "failed to convert %s to float", pair->second.c_str());
            return default_value;
        }
    }
}

void ParamPackage::Set(const std::string& key, const std::string& value) {
    data[key] = value;
}

void ParamPackage::Set(const std::string& key, int value) {
    data[key] = std::to_string(value);
}

void ParamPackage::Set(const std::string& key, float value) {
    data[key] = std::to_string(value);
}

} // namespace Common
