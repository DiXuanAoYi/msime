#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace Speech
{
inline nlohmann::json CloudVoiceCatalog(const std::string &provider)
{
    using nlohmann::json;
    json voices = json::array();
    if (provider == "azure")
    {
        voices.push_back({{"id", "zh-CN-XiaoxiaoNeural"}, {"name", "晓晓 · 普通话"}, {"language", "zh"}});
        voices.push_back({{"id", "wuu-CN-XiaotongNeural"},
                          {"name", "晓彤 · 上海话"},
                          {"language", "zh"},
                          {"dialect", "上海话（吴语）"}});
        voices.push_back(
            {{"id", "zh-HK-HiuMaanNeural"}, {"name", "晓曼 · 粤语"}, {"language", "zh"}, {"dialect", "粤语"}});
        for (const auto &[code, name] :
             std::initializer_list<std::pair<const char *, const char *>>{{"shaanxi", "陕西话"},
                                                                          {"sichuan", "四川话"},
                                                                          {"shanxi", "山西话"},
                                                                          {"anhui", "安徽话"},
                                                                          {"hunan", "湖南话"},
                                                                          {"gansu", "甘肃话"},
                                                                          {"shandong", "山东话"},
                                                                          {"henan", "河南话"},
                                                                          {"liaoning", "辽宁话"}})
            voices.push_back({{"id", std::string("zh-CN-XiaoxiaoDialectsNeural|zh-CN-") + code},
                              {"name", std::string("晓晓方言 · ") + name},
                              {"language", "zh"},
                              {"dialect", name}});
        voices.push_back({{"id", "zh-CN-XiaoxiaoDialectsNeural|nan-CN"},
                          {"name", "晓晓方言 · 闽南语"},
                          {"language", "zh"},
                          {"dialect", "闽南语"}});
        voices.push_back({{"id", "en-US-JennyNeural"}, {"name", "Jenny · 美式英语"}, {"language", "en"}});
    }
    else if (provider == "volcengine")
    {
        voices.push_back({{"id", "zh_female_vv_uranus_bigtts"}, {"name", "Vivi · 普通话"}, {"language", "zh"}});
        for (const auto &[code, name] : std::initializer_list<std::pair<const char *, const char *>>{
                 {"dongbei", "东北话"}, {"shaanxi", "陕西话"}, {"sichuan", "四川话"}})
            voices.push_back({{"id", std::string("zh_female_vv_uranus_bigtts|") + code},
                              {"name", std::string("Vivi · ") + name},
                              {"language", "zh"},
                              {"dialect", name}});
        voices.push_back({{"id", "en_female_amanda_mars_bigtts"}, {"name", "Amanda · 英语"}, {"language", "en"}});
    }
    return {{"id", provider},
            {"voices", voices},
            {"dialectNote",
             "方言由所选服务音色与资源权限决定；Vivi 预置仅列已核实的东北、陕西、四川。火山其他方言需搭配控制台"
             "明确支持该方言的音色，可填写 音色ID|方言代码。苏州话尚无已核实的内置音色，上海话不能替代苏州话。藏语请"
             "选本地 MMS 藏语模型并输入藏文。另有按需下载的维吾尔语（阿拉伯文字）、蒙古语（西里尔文字）和哈萨克语；"
             "自部署兼容服务可使用已登记的参考音色 ID，但音色克隆不等于新增语言或方言能力。"}};
}
} // namespace Speech
