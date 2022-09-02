#include "format.h"
#include "pretty_table.h"

#include <util/string/vector.h>
#include <library/cpp/json/json_prettifier.h>

#include <ydb/public/lib/json_value/ydb_json_value.h>

namespace NYdb {
namespace NConsoleClient {

namespace {
    THashMap<EOutputFormat, TString> InputFormatDescriptions = {
        { EOutputFormat::JsonUnicode, "Input in json format, binary strings are decoded with unicode characters" },
        { EOutputFormat::JsonBase64, "Input in json format, binary strings are decoded with base64" },
        { EOutputFormat::Csv, "Input in csv format" },
        { EOutputFormat::Tsv, "Input in tsv format" },
    };

    THashMap<EOutputFormat, TString> FormatDescriptions = {
        { EOutputFormat::Pretty, "Human readable output" },
        { EOutputFormat::Json, "Output in json format" },
        { EOutputFormat::JsonUnicode, "Output in json format, binary strings are encoded with unicode characters. "
                                      "Every row is a separate json on a separate line." },
        { EOutputFormat::JsonUnicodeArray, "Output in json format, binary strings are encoded with unicode characters. "
                                           "Every resultset is a json array of rows. "
                                           "Every row is a separate json on a separate line." },
        { EOutputFormat::JsonBase64, "Output in json format, binary strings are encoded with base64. "
                                     "Every row is a separate json on a separate line." },
        { EOutputFormat::JsonBase64Array, "Output in json format, binary strings are encoded with base64. "
                                           "Every resultset is a json array of rows. "
                                           "Every row is a separate json on a separate line." },
        { EOutputFormat::JsonRawArray, "Output in json format, binary strings are not encoded."
                                        "Every resultset is a json array of rows. "
                                        "Every row is a separate binary data on a separate line"},
        { EOutputFormat::ProtoJsonBase64, "Output result protobuf in json format, binary strings are encoded with base64" },
        { EOutputFormat::Csv, "Output in csv format" },
    };

    THashMap<EMessagingFormat, TString> MessagingFormatDescriptions = {
        { EMessagingFormat::Pretty, "Human readable output with metadata." },
        { EMessagingFormat::SingleMessage, "Single message."}, // TODO(shmel1k@): improve
        { EMessagingFormat::NewlineDelimited, "Newline delimited stream of messages."}, // TODO(shmel1k@): improve
        { EMessagingFormat::Concatenated, "Concatenated output stream of messages."}, // TODO(shmel1k@): improve,
        { EMessagingFormat::JsonStreamConcat, "Concatenated Json stream of envelopes with metadata and messages in the ""body"" attribute." }, // TODO(shmel1k@): improve,
        { EMessagingFormat::JsonArray, "Json array of envelopes with metadata and messages in the ""body"" attribute." }, // TODO(shmel1k@): improve,
    };
}

void TCommandWithResponseHeaders::PrintResponseHeader(const TStatus& status) {
    if (!ShowHeaders)
        return;

    PrintResponseHeaderPretty(status);
}

void TCommandWithResponseHeaders::PrintResponseHeaderPretty(const TStatus& status) {
    const auto columnNames = TVector<TString>{"meta key", "meta value"};
    TPrettyTable table(columnNames);

    const auto headers = status.GetResponseMetadata();
    for (const auto& h : headers) {
        auto& row = table.AddRow();
        row.Column(0, h.first);
        row.Column(1, h.second);
    }

    Cout << table;
}

const TString TCommandWithResponseHeaders::ResponseHeadersHelp = "Show response metadata for ydb call";

// Deprecated
void TCommandWithFormat::AddDeprecatedJsonOption(TClientCommand::TConfig& config, const TString& description) {
    config.Opts->AddLongOption("json", description).NoArgument()
        .StoreValue(&OutputFormat, EOutputFormat::Json).StoreValue(&DeprecatedOptionUsed, true)
        .Hidden();
}

void TCommandWithFormat::AddInputFormats(TClientCommand::TConfig& config, const TVector<EOutputFormat>& allowedFormats) {
    TStringStream description;
    description << "Input format. Available options: ";
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);
    for (const auto& format : allowedFormats) {
        auto findResult = InputFormatDescriptions.find(format);
        Y_VERIFY(findResult != InputFormatDescriptions.end(),
            "Couldn't find description for %s input format", (TStringBuilder() << format).c_str());
        description << "\n  " << colors.BoldColor() << format << colors.OldColor()
            << "\n    " << findResult->second;
    }
    config.Opts->AddLongOption("input-format", description.Str())
        .RequiredArgument("STRING").StoreResult(&InputFormat);
    AllowedInputFormats = allowedFormats;
}

void TCommandWithFormat::AddFormats(TClientCommand::TConfig& config, const TVector<EOutputFormat>& allowedFormats) {
    TStringStream description;
    description << "Output format. Available options: ";
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);
    for (const auto& format : allowedFormats) {
        auto findResult = FormatDescriptions.find(format);
        Y_VERIFY(findResult != FormatDescriptions.end(),
            "Couldn't find description for %s output format", (TStringBuilder() << format).c_str());
        description << "\n  " << colors.BoldColor() << format << colors.OldColor()
            << "\n    " << findResult->second;
    }
    config.Opts->AddLongOption("format", description.Str())
        .RequiredArgument("STRING").StoreResult(&OutputFormat);
    AllowedFormats = allowedFormats;
}

void TCommandWithFormat::AddMessagingFormats(TClientCommand::TConfig& config, const TVector<EMessagingFormat>& allowedFormats) {
    TStringStream description;
    description << "Client-side format. Available options: ";
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);
    for (const auto& format : allowedFormats) {
        auto findResult = MessagingFormatDescriptions.find(format);
        Y_VERIFY(findResult != MessagingFormatDescriptions.end(),
            "Couldn't find description for %s output format", (TStringBuilder() << format).c_str());
        description << "\n  " << colors.BoldColor() << format << colors.OldColor()
            << "\n    " << findResult->second;
    }
    config.Opts->AddLongOption("format", description.Str())
        .DefaultValue( "single-message" )
        .RequiredArgument("STRING").StoreResult(&MessagingFormat);
    AllowedMessagingFormats = allowedFormats;
}

void TCommandWithFormat::ParseFormats() {
    if (InputFormat != EOutputFormat::Default
            && std::find(AllowedInputFormats.begin(), AllowedInputFormats.end(), InputFormat) == AllowedInputFormats.end()) {
        throw TMisuseException() << "Input format " << InputFormat << " is not available for this command";
    }


    if (OutputFormat == EOutputFormat::Default || DeprecatedOptionUsed) {
        return;
    }
    if (std::find(AllowedFormats.begin(), AllowedFormats.end(), OutputFormat) == AllowedFormats.end()) {
        throw TMisuseException() << "Output format " << OutputFormat << " is not available for this command";
    }
}

void TCommandWithFormat::ParseMessagingFormats() {
    if (MessagingFormat != EMessagingFormat::SingleMessage
            && std::find(AllowedMessagingFormats.begin(), AllowedMessagingFormats.end(), MessagingFormat) == AllowedMessagingFormats.end()) {
        throw TMisuseException() << "Messaging format " << MessagingFormat << " is not available for this command";
    }
}


void TQueryPlanPrinter::Print(const TString& plan) {
    switch (Format) {
        case EOutputFormat::Default:
        case EOutputFormat::Pretty: {
            NJson::TJsonValue planJson;
            NJson::ReadJsonTree(plan, &planJson, true);

            Y_ENSURE(planJson.GetMapSafe().contains("meta"));
            const auto& meta = planJson.GetMapSafe().at("meta");

            Y_ENSURE(meta.GetMapSafe().contains("type"));
            if (meta.GetMapSafe().at("type").GetStringSafe() == "script") {
                Y_ENSURE(planJson.GetMapSafe().contains("queries"));
                const auto& queries = planJson.GetMapSafe().at("queries").GetArraySafe();
                for (size_t queryId = 0; queryId < queries.size(); ++queryId) {
                    const auto& query = queries[queryId];
                    Cout << "Query " << queryId << ":" << Endl;
                    PrintPretty(query);
                }
            } else {
                PrintPretty(planJson);
            }

            break;
        }
        case EOutputFormat::JsonUnicode:
        case EOutputFormat::JsonBase64:
            PrintJson(plan);
            break;
        default:
            throw TMisuseException() << "This command doesn't support " << Format << " output format";
    }
}

void TQueryPlanPrinter::PrintJson(const TString& plan) {
    Cout << NJson::PrettifyJson(plan, true) << Endl;
}

void TQueryPlanPrinter::PrintPretty(const NJson::TJsonValue& plan) {
    if (plan.GetMapSafe().contains("Plan")) {
        const auto& queryPlan = plan.GetMapSafe().at("Plan").GetMapSafe();
        Y_ENSURE(queryPlan.contains("Plans"));

        TVector<TString> offsets;
        for (const auto& subplan : queryPlan.at("Plans").GetArraySafe()) {
            PrintPrettyImpl(subplan, offsets);
        }
    } else { /* old format plan */
        PrintJson(plan.GetStringRobust());
    }
}

void TQueryPlanPrinter::PrintPrettyImpl(const NJson::TJsonValue& plan, TVector<TString>& offsets) {
    static const TString edge = "|  ";
    static const TString noEdge = "   ";
    static const TString edgeBranch = "├──";
    static const TString edgeBranchLast = "└──";

    TStringBuilder prefix;
    TStringBuilder headerPrefix;
    for (const auto& offset : offsets) {
        if (&offset != &offsets.back()) {
            prefix << offset;
        }
        headerPrefix << offset;
    }

    if (!offsets.empty()) {
        bool last = (offsets.back() == edge);
        prefix << (last ? edgeBranch : edgeBranchLast);
    }

    const auto& node = plan.GetMapSafe();

    if (node.contains("Operators")) {
        for (const auto& op : node.at("Operators").GetArraySafe()) {
            TVector<TString> info;
            for (const auto& [key, value] : op.GetMapSafe()) {
                if (key != "Name") {
                    info.emplace_back(TStringBuilder() << key << ": " << JsonToString(value));
                }
            }

            if (info.empty()) {
                Cout << prefix << op.GetMapSafe().at("Name").GetString() << Endl;
            } else {
                Cout << prefix << op.GetMapSafe().at("Name").GetString()
                     << " (" << JoinStrings(info, ", ") << ")" << Endl;
            }
        }
    } else if (node.contains("PlanNodeType") && node.at("PlanNodeType").GetString() == "Connection") {
        Cout << prefix << "<" << node.at("Node Type").GetString() << ">" << Endl;
    } else {
        Cout << prefix << node.at("Node Type").GetString() << Endl;
    }

    static const THashSet<TString> requiredFields = {"CTE Name", "Tables"};
    for (const auto& [key, value] : node) {
        if (requiredFields.contains(key)) {
            Cout << headerPrefix << key << ": " << JsonToString(value) << Endl;
        }
    }

    if (AnalyzeMode && node.contains("Stats")) {
        NColorizer::TColors colors = NColorizer::AutoColors(Cout);
        for (const auto& [key, value] : node.at("Stats").GetMapSafe()) {
            Cout << headerPrefix << colors.Yellow() << key << ": " << colors.Cyan()
                 << JsonToString(value) << colors.Default() << Endl;
        }
    }

    if (node.contains("Plans")) {
        const auto& plans = node.at("Plans").GetArraySafe();
        for (const auto& subplan : plans) {
            offsets.push_back(&subplan != &plans.back() ? edge : noEdge);
            PrintPrettyImpl(subplan, offsets);
            offsets.pop_back();
        }
    }
}

TString TQueryPlanPrinter::JsonToString(const NJson::TJsonValue& jsonValue) {
    TStringBuilder str;

    if (jsonValue.IsString()) {
        str << jsonValue.GetString();
    } else if (jsonValue.IsArray()) {
        str << "[";
        const auto& array = jsonValue.GetArraySafe();
        for (auto it = array.begin(); it != array.end(); ++it) {
            str << (it != array.begin() ? ", " : "")
                << JsonToString(*it);
        }
        str << "]";
    } else if (jsonValue.IsMap()) {
        str << "{";
        const auto& map = jsonValue.GetMapSafe();
        for (auto it = map.begin(); it != map.end(); ++it) {
            str << (it != map.begin() ? ", " : "")
                << it->first << ": " << JsonToString(it->second);
        }
        str << "}";
    } else {
        str << jsonValue;
    }

    return str;
}


TResultSetPrinter::TResultSetPrinter(EOutputFormat format, std::function<bool()> isInterrupted)
    : Format(format)
    , IsInterrupted(isInterrupted)
{}

TResultSetPrinter::~TResultSetPrinter() {
    if (PrintedSomething && !IsInterrupted()) {
        EndResultSet();
    }
}

void TResultSetPrinter::Print(const TResultSet& resultSet) {
    if (FirstPart) {
        BeginResultSet();
    } else {
        EndLineBeforeNextResult();
    }
    PrintedSomething = true;

    switch (Format) {
    case EOutputFormat::Default:
    case EOutputFormat::Pretty:
        PrintPretty(resultSet);
        break;
    case EOutputFormat::JsonUnicodeArray:
        PrintJsonArray(resultSet, EBinaryStringEncoding::Unicode);
        break;
    case EOutputFormat::JsonUnicode:
        FormatResultSetJson(resultSet, &Cout, EBinaryStringEncoding::Unicode);
        break;
    case EOutputFormat::JsonBase64Array:
        PrintJsonArray(resultSet, EBinaryStringEncoding::Base64);
        break;
    case EOutputFormat::JsonBase64:
        FormatResultSetJson(resultSet, &Cout, EBinaryStringEncoding::Base64);
        break;
    case EOutputFormat::Csv:
        PrintCsv(resultSet);
        break;
    default:
        throw TMisuseException() << "This command doesn't support " << Format << " output format";
    }
}

void TResultSetPrinter::Reset() {
    if (PrintedSomething) {
        EndResultSet();
        FirstPart = true;
    }
}

void TResultSetPrinter::BeginResultSet() {
    switch (Format) {
    case EOutputFormat::JsonUnicodeArray:
    case EOutputFormat::JsonBase64Array:
        Cout << '[';
        break;
    default:
        break;
    }
}

void TResultSetPrinter::EndResultSet() {
    switch (Format) {
    case EOutputFormat::JsonUnicodeArray:
    case EOutputFormat::JsonBase64Array:
        Cout << ']' << Endl;
        break;
    default:
        break;
    }
}

void TResultSetPrinter::EndLineBeforeNextResult() {
    switch (Format) {
    case EOutputFormat::JsonUnicodeArray:
    case EOutputFormat::JsonBase64Array:
        Cout << ',' << Endl;
        break;
    default:
        break;
    }
}

void TResultSetPrinter::PrintPretty(const TResultSet& resultSet) {
    const TVector<TColumn>& columns = resultSet.GetColumnsMeta();
    TResultSetParser parser(resultSet);
    TVector<TString> columnNames;
    for (const auto& column : columns) {
        columnNames.push_back(column.Name);
    }

    TPrettyTableConfig tableConfig;
    if (!FirstPart) {
        tableConfig.WithoutHeader();
    }
    TPrettyTable table(columnNames, tableConfig);

    while (parser.TryNextRow()) {
        auto& row = table.AddRow();
        for (ui32 i = 0; i < columns.size(); ++i) {
            row.Column(i, FormatValueJson(parser.GetValue(i), EBinaryStringEncoding::Unicode));
        }
    }

    Cout << table;
}

void TResultSetPrinter::PrintJsonArray(const TResultSet& resultSet, EBinaryStringEncoding encoding) {
    auto columns = resultSet.GetColumnsMeta();

    TResultSetParser parser(resultSet);
    bool firstRow = true;
    while (parser.TryNextRow()) {
        if (!firstRow) {
            EndLineBeforeNextResult();
        } else {
            firstRow = false;
        }
        NJsonWriter::TBuf writer(NJsonWriter::HEM_UNSAFE, &Cout);
        FormatResultRowJson(parser, columns, writer, encoding);
    }
}

void TResultSetPrinter::PrintCsv(const TResultSet& resultSet) {
    const TVector<TColumn>& columns = resultSet.GetColumnsMeta();
    TResultSetParser parser(resultSet);
    while (parser.TryNextRow()) {
        for (ui32 i = 0; i < columns.size(); ++i) {
            Cout << FormatValueJson(parser.GetValue(i), EBinaryStringEncoding::Unicode);
            if (i < columns.size() - 1) {
                Cout << ",";
            }
        }
        Cout << Endl;
    }
}

}
}
