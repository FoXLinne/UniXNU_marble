// SPDX-License-Identifier: MIT
#pragma once
#include <set>
#include <string>
#include <vector>
namespace prime {
inline std::vector<std::string> cacheBuildIds(const std::string &runtime,const std::string &compiler,
                                             const std::string &image,const std::string &compatible) {
    std::vector<std::string> ids{runtime + (compiler.empty()?"-" + image:"-tu-" + compiler)};
    const std::string images=image+";"+compatible;
    for(size_t at=0;at<images.size();) {
        size_t end=images.find(';',at);if(end==std::string::npos)end=images.size();
        const std::string uuid=images.substr(at,end-at);
        if(uuid.size()==32 && uuid.find_first_not_of("0123456789abcdef")==std::string::npos) {
            const std::string id=runtime+"-"+uuid;
            bool found=false;for(const auto &known:ids)if(known==id)found=true;
            if(!found)ids.push_back(id);
        }
        at=end+1;
    }
    return ids;
}
inline bool progressContains(const std::set<std::string> &done,const std::string &prefix,
                             const std::vector<std::string> &builds) {
    for(const auto &build:builds)if(done.count(prefix+"\t"+build))return true;
    return false;
}
}
