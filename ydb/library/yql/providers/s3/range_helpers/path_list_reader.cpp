#include "path_list_reader.h"

#include <ydb/library/yql/providers/s3/range_helpers/file_tree_builder.h>

#include <ydb/library/yql/providers/common/provider/yql_provider_names.h>
#include <ydb/library/yql/utils/yql_panic.h>

#include <library/cpp/protobuf/util/pb_io.h>
#include <google/protobuf/text_format.h>

#include <util/stream/mem.h>
#include <util/stream/str.h>

namespace NYql::NS3Details {

static void BuildPathsFromTree(const google::protobuf::RepeatedPtrField<NYql::NS3::TRange::TPath>& children, TPathList& paths, TString& currentPath, size_t currentDepth = 0) {
    if (children.empty()) {
        return;
    }
    if (currentDepth) {
        currentPath += '/';
    }
    for (const auto& path : children) {
        const size_t prevSize = currentPath.size();
        currentPath += path.GetName();
        if (path.GetRead()) {
            paths.emplace_back(currentPath, path.GetSize());
        }
        BuildPathsFromTree(path.GetChildren(), paths, currentPath, currentDepth + 1);
        currentPath.resize(prevSize);
    }
}

void ReadPathsList(const NS3::TSource& sourceDesc, const THashMap<TString, TString>& taskParams, TPathList& paths, ui64& startPathIndex) {
    if (const auto taskParamsIt = taskParams.find(S3ProviderName); taskParamsIt != taskParams.cend()) {
        NS3::TRange range;
        TStringInput input(taskParamsIt->second);
        range.Load(&input);
        startPathIndex = range.GetStartPathIndex();

        // Modern way
        if (range.PathsSize()) {
            TString buf;
            return BuildPathsFromTree(range.GetPaths(), paths, buf);
        }

        std::unordered_map<TString, size_t> map(sourceDesc.GetDeprecatedPath().size());
        for (auto i = 0; i < sourceDesc.GetDeprecatedPath().size(); ++i) {
            map.emplace(sourceDesc.GetDeprecatedPath().Get(i).GetPath(), sourceDesc.GetDeprecatedPath().Get(i).GetSize());
        }

        for (auto i = 0; i < range.GetDeprecatedPath().size(); ++i) {
            const auto& path = range.GetDeprecatedPath().Get(i);
            auto it = map.find(path);
            YQL_ENSURE(it != map.end());
            paths.emplace_back(path, it->second);
        }
    } else {
        for (auto i = 0; i < sourceDesc.GetDeprecatedPath().size(); ++i) {
            paths.emplace_back(sourceDesc.GetDeprecatedPath().Get(i).GetPath(), sourceDesc.GetDeprecatedPath().Get(i).GetSize());
        }
    }
}

void PackPathsList(const TPathList& paths, TString& packed, bool& isTextEncoded) {
    TFileTreeBuilder builder;
    for (auto& item : paths) {
        builder.AddPath(std::get<0>(item), std::get<1>(item));
    }
    NS3::TRange range;
    builder.Save(&range);

    isTextEncoded = range.GetPaths().size() < 100;
    if (isTextEncoded) {
        google::protobuf::TextFormat::PrintToString(range, &packed);
    } else {
        packed = range.SerializeAsString();
    }
}

void UnpackPathsList(TStringBuf packed, bool isTextEncoded, TPathList& paths) {
    NS3::TRange range;
    TMemoryInput inputStream(packed);
    if (isTextEncoded) {
        ParseFromTextFormat(inputStream, range);
    } else {
        range.ParseFromArcadiaStream(&inputStream);
    }

    TString buf;
    BuildPathsFromTree(range.GetPaths(), paths, buf);
}

} // namespace NYql::NS3Details
