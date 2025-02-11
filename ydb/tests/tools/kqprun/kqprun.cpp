#include "src/kqp_runner.h"

#include <cstdio>

#include <contrib/libs/protobuf/src/google/protobuf/text_format.h>

#include <library/cpp/colorizer/colors.h>
#include <library/cpp/getopt/last_getopt.h>
#include <library/cpp/getopt/small/modchooser.h>

#include <util/stream/file.h>
#include <util/system/env.h>

#include <ydb/core/base/backtrace.h>

#include <ydb/library/aclib/aclib.h>
#include <ydb/library/yaml_config/yaml_config.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <ydb/library/yql/providers/yt/gateway/file/yql_yt_file.h>
#include <ydb/library/yql/providers/yt/gateway/file/yql_yt_file_comp_nodes.h>
#include <ydb/library/yql/providers/yt/lib/yt_download/yt_download.h>
#include <yql/essentials/public/udf/udf_static_registry.h>


struct TExecutionOptions {
    enum class EExecutionCase {
        GenericScript,
        GenericQuery,
        YqlScript,
        AsyncQuery
    };

    std::vector<TString> ScriptQueries;
    TString SchemeQuery;
    bool UseTemplates = false;

    ui32 LoopCount = 1;
    TDuration LoopDelay;
    bool ContinueAfterFail = false;

    bool ForgetExecution = false;
    std::vector<EExecutionCase> ExecutionCases;
    std::vector<NKikimrKqp::EQueryAction> ScriptQueryActions;
    std::vector<TString> Databases;
    std::vector<TString> TraceIds;
    std::vector<TString> PoolIds;
    std::vector<TString> UserSIDs;
    std::vector<TDuration> Timeouts;
    ui64 ResultsRowsLimit = 0;

    const TString DefaultTraceId = "kqprun";

    bool HasResults() const {
        for (size_t i = 0; i < ScriptQueries.size(); ++i) {
            if (GetScriptQueryAction(i) != NKikimrKqp::EQueryAction::QUERY_ACTION_EXECUTE) {
                continue;
            }
            if (GetExecutionCase(i) != EExecutionCase::AsyncQuery) {
                return true;
            }
        }
        return false;
    }

    bool HasExecutionCase(EExecutionCase executionCase) const {
        if (ExecutionCases.empty()) {
            return executionCase == EExecutionCase::GenericScript;
        }
        return std::find(ExecutionCases.begin(), ExecutionCases.end(), executionCase) != ExecutionCases.end();
    }

    EExecutionCase GetExecutionCase(size_t index) const {
        return GetValue(index, ExecutionCases, EExecutionCase::GenericScript);
    }

    NKikimrKqp::EQueryAction GetScriptQueryAction(size_t index) const {
        return GetValue(index, ScriptQueryActions, NKikimrKqp::EQueryAction::QUERY_ACTION_EXECUTE);
    }

    NKqpRun::TRequestOptions GetSchemeQueryOptions() const {
        TString sql = SchemeQuery;
        if (UseTemplates) {
            ReplaceYqlTokenTemplate(sql);
        }

        return {
            .Query = sql,
            .Action = NKikimrKqp::EQueryAction::QUERY_ACTION_EXECUTE,
            .TraceId = DefaultTraceId,
            .PoolId = "",
            .UserSID = BUILTIN_ACL_ROOT,
            .Database = "",
            .Timeout = TDuration::Zero()
        };
    }

    NKqpRun::TRequestOptions GetScriptQueryOptions(size_t index, size_t queryId, TInstant startTime) const {
        Y_ABORT_UNLESS(index < ScriptQueries.size());

        TString sql = ScriptQueries[index];
        if (UseTemplates) {
            ReplaceYqlTokenTemplate(sql);
            SubstGlobal(sql, "${QUERY_ID}", ToString(queryId));
        }

        return {
            .Query = sql,
            .Action = GetScriptQueryAction(index),
            .TraceId = TStringBuilder() << GetValue(index, TraceIds, DefaultTraceId) << "-" << startTime.ToString(),
            .PoolId = GetValue(index, PoolIds, TString()),
            .UserSID = GetValue(index, UserSIDs, TString(BUILTIN_ACL_ROOT)),
            .Database = GetValue(index, Databases, TString()),
            .Timeout = GetValue(index, Timeouts, TDuration::Zero())
        };
    }

    void Validate(const NKqpRun::TRunnerOptions& runnerOptions) const {
        if (!SchemeQuery && ScriptQueries.empty() && !runnerOptions.YdbSettings.MonitoringEnabled && !runnerOptions.YdbSettings.GrpcEnabled) {
            ythrow yexception() << "Nothing to execute and is not running as daemon";
        }

        ValidateOptionsSizes();
        ValidateSchemeQueryOptions(runnerOptions);
        ValidateScriptExecutionOptions(runnerOptions);
        ValidateAsyncOptions(runnerOptions.YdbSettings.AsyncQueriesSettings);
        ValidateTraceOpt(runnerOptions.TraceOptType);
    }

private:
    void ValidateOptionsSizes() const {
        const auto checker = [numberQueries = ScriptQueries.size()](size_t checkSize, const TString& optionName) {
            if (checkSize > numberQueries) {
                ythrow yexception() << "Too many " << optionName << ". Specified " << checkSize << ", when number of queries is " << numberQueries;
            }
        };

        checker(ExecutionCases.size(), "execution cases");
        checker(ScriptQueryActions.size(), "script query actions");
        checker(Databases.size(), "databases");
        checker(TraceIds.size(), "trace ids");
        checker(PoolIds.size(), "pool ids");
        checker(UserSIDs.size(), "user SIDs");
        checker(Timeouts.size(), "timeouts");
    }

    void ValidateSchemeQueryOptions(const NKqpRun::TRunnerOptions& runnerOptions) const {
        if (SchemeQuery) {
            return;
        }
        if (runnerOptions.SchemeQueryAstOutput) {
            ythrow yexception() << "Scheme query AST output can not be used without scheme query";
        }
    }

    void ValidateScriptExecutionOptions(const NKqpRun::TRunnerOptions& runnerOptions) const {
        if (runnerOptions.YdbSettings.SameSession && HasExecutionCase(EExecutionCase::AsyncQuery)) {
            ythrow yexception() << "Same session can not be used with async quries";
        }

        // Script specific
        if (HasExecutionCase(EExecutionCase::GenericScript)) {
            return;
        }
        if (ForgetExecution) {
            ythrow yexception() << "Forget execution can not be used without generic script queries";
        }
        if (runnerOptions.ScriptCancelAfter) {
            ythrow yexception() << "Cancel after can not be used without generic script queries";
        }

        // Script/Query specific
        if (HasExecutionCase(EExecutionCase::GenericQuery)) {
            return;
        }
        if (ResultsRowsLimit) {
            ythrow yexception() << "Result rows limit can not be used without script queries";
        }
        if (runnerOptions.InProgressStatisticsOutputFile) {
            ythrow yexception() << "Script statistics can not be used without script queries";
        }

        // Common specific
        if (HasExecutionCase(EExecutionCase::YqlScript)) {
            return;
        }
        if (runnerOptions.ScriptQueryAstOutput) {
            ythrow yexception() << "Script query AST output can not be used without script/yql queries";
        }
        if (runnerOptions.ScriptQueryPlanOutput) {
            ythrow yexception() << "Script query plan output can not be used without script/yql queries";
        }
        if (runnerOptions.YdbSettings.SameSession) {
            ythrow yexception() << "Same session can not be used without script/yql queries";
        }
    }

    void ValidateAsyncOptions(const NKqpRun::TAsyncQueriesSettings& asyncQueriesSettings) const {
        if (asyncQueriesSettings.InFlightLimit && !HasExecutionCase(EExecutionCase::AsyncQuery)) {
            ythrow yexception() << "In flight limit can not be used without async queries";
        }

        NColorizer::TColors colors = NColorizer::AutoColors(Cout);
        if (LoopCount && asyncQueriesSettings.InFlightLimit && asyncQueriesSettings.InFlightLimit > ScriptQueries.size() * LoopCount) {
            Cout << colors.Red() << "Warning: inflight limit is " << asyncQueriesSettings.InFlightLimit << ", that is larger than max possible number of queries " << ScriptQueries.size() * LoopCount << colors.Default() << Endl;
        }
    }

    void ValidateTraceOpt(NKqpRun::TRunnerOptions::ETraceOptType traceOptType) const {
        switch (traceOptType) {
            case NKqpRun::TRunnerOptions::ETraceOptType::Scheme: {
                if (!SchemeQuery) {
                    ythrow yexception() << "Trace opt type scheme cannot be used without scheme query";
                }
                break;
            }
            case NKqpRun::TRunnerOptions::ETraceOptType::Script: {
                if (ScriptQueries.empty()) {
                    ythrow yexception() << "Trace opt type script cannot be used without script queries";
                }
            }
            case NKqpRun::TRunnerOptions::ETraceOptType::All: {
                if (!SchemeQuery && ScriptQueries.empty()) {
                    ythrow yexception() << "Trace opt type all cannot be used without any queries";
                }
            }
            case NKqpRun::TRunnerOptions::ETraceOptType::Disabled: {
                break;
            }
        }
    }

private:
    template <typename TValue>
    static TValue GetValue(size_t index, const std::vector<TValue>& values, TValue defaultValue) {
        if (values.empty()) {
            return defaultValue;
        }
        return values[std::min(index, values.size() - 1)];
    }

    static void ReplaceYqlTokenTemplate(TString& sql) {
        const TString variableName = TStringBuilder() << "${" << NKqpRun::YQL_TOKEN_VARIABLE << "}";
        if (const TString& yqlToken = GetEnv(NKqpRun::YQL_TOKEN_VARIABLE)) {
            SubstGlobal(sql, variableName, yqlToken);
        } else if (sql.Contains(variableName)) {
            ythrow yexception() << "Failed to replace ${YQL_TOKEN} template, please specify YQL_TOKEN environment variable\n";
        }
    }
};


void RunArgumentQuery(size_t index, size_t queryId, TInstant startTime, const TExecutionOptions& executionOptions, NKqpRun::TKqpRunner& runner) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    switch (executionOptions.GetExecutionCase(index)) {
        case TExecutionOptions::EExecutionCase::GenericScript: {
            if (!runner.ExecuteScript(executionOptions.GetScriptQueryOptions(index, queryId, startTime))) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Script execution failed";
            }
            Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Fetching script results..." << colors.Default() << Endl;
            if (!runner.FetchScriptResults()) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Fetch script results failed";
            }
            if (executionOptions.ForgetExecution) {
                Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Forgetting script execution operation..." << colors.Default() << Endl;
                if (!runner.ForgetExecutionOperation()) {
                    ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Forget script execution operation failed";
                }
            }
            break;
        }

        case TExecutionOptions::EExecutionCase::GenericQuery: {
            if (!runner.ExecuteQuery(executionOptions.GetScriptQueryOptions(index, queryId, startTime))) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Query execution failed";
            }
            break;
        }

        case TExecutionOptions::EExecutionCase::YqlScript: {
            if (!runner.ExecuteYqlScript(executionOptions.GetScriptQueryOptions(index, queryId, startTime))) {
                ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Yql script execution failed";
            }
            break;
        }

        case TExecutionOptions::EExecutionCase::AsyncQuery: {
            runner.ExecuteQueryAsync(executionOptions.GetScriptQueryOptions(index, queryId, startTime));
            break;
        }
    }
}


void RunArgumentQueries(const TExecutionOptions& executionOptions, NKqpRun::TKqpRunner& runner) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    if (executionOptions.SchemeQuery) {
        Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Executing scheme query..." << colors.Default() << Endl;
        if (!runner.ExecuteSchemeQuery(executionOptions.GetSchemeQueryOptions())) {
            ythrow yexception() << TInstant::Now().ToIsoStringLocal() << " Scheme query execution failed";
        }
    }

    const size_t numberQueries = executionOptions.ScriptQueries.size();
    const size_t numberLoops = executionOptions.LoopCount;
    for (size_t queryId = 0; queryId < numberQueries * numberLoops || numberLoops == 0; ++queryId) {
        size_t id = queryId % numberQueries;
        if (id == 0 && queryId > 0) {
            Sleep(executionOptions.LoopDelay);
        }

        const TInstant startTime = TInstant::Now();
        if (executionOptions.GetExecutionCase(id) != TExecutionOptions::EExecutionCase::AsyncQuery) {
            Cout << colors.Yellow() << startTime.ToIsoStringLocal() << " Executing script";
            if (numberQueries > 1) {
                Cout << " " << id;
            }
            if (numberLoops != 1) {
                Cout << ", loop " << queryId / numberQueries;
            }
            Cout << "..." << colors.Default() << Endl;
        }

        try {
            RunArgumentQuery(id, queryId, startTime, executionOptions, runner);
        } catch (const yexception& exception) {
            if (executionOptions.ContinueAfterFail) {
                Cerr << colors.Red() <<  CurrentExceptionMessage() << colors.Default() << Endl;
            } else {
                throw exception;
            }
        }
    }
    runner.FinalizeRunner();

    if (executionOptions.HasResults()) {
        try {
            runner.PrintScriptResults();
        } catch (...) {
            ythrow yexception() << "Failed to print script results, reason:\n" <<  CurrentExceptionMessage();
        }
    }
}


void RunAsDaemon() {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Initialization finished" << colors.Default() << Endl;
    while (true) {
        Sleep(TDuration::Seconds(1));
    }
}


void RunScript(const TExecutionOptions& executionOptions, const NKqpRun::TRunnerOptions& runnerOptions) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cout);

    Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Initialization of kqp runner..." << colors.Default() << Endl;
    NKqpRun::TKqpRunner runner(runnerOptions);

    try {
        RunArgumentQueries(executionOptions, runner);
    } catch (const yexception& exception) {
        if (runnerOptions.YdbSettings.MonitoringEnabled) {
            Cerr << colors.Red() <<  CurrentExceptionMessage() << colors.Default() << Endl;
        } else {
            throw exception;
        }
    }

    if (runnerOptions.YdbSettings.MonitoringEnabled || runnerOptions.YdbSettings.GrpcEnabled) {
        RunAsDaemon();
    }

    Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Finalization of kqp runner..." << colors.Default() << Endl;
}


TIntrusivePtr<NKikimr::NMiniKQL::IMutableFunctionRegistry> CreateFunctionRegistry(const TString& udfsDirectory, TVector<TString> udfsPaths, bool excludeLinkedUdfs) {
    if (!udfsDirectory.empty() || !udfsPaths.empty()) {
        NColorizer::TColors colors = NColorizer::AutoColors(Cout);
        Cout << colors.Yellow() << TInstant::Now().ToIsoStringLocal() << " Fetching udfs..." << colors.Default() << Endl;
    }

    NKikimr::NMiniKQL::FindUdfsInDir(udfsDirectory, &udfsPaths);
    auto functionRegistry = NKikimr::NMiniKQL::CreateFunctionRegistry(&PrintBackTrace, NKikimr::NMiniKQL::CreateBuiltinRegistry(), false, udfsPaths)->Clone();

    if (excludeLinkedUdfs) {
        for (const auto& wrapper : NYql::NUdf::GetStaticUdfModuleWrapperList()) {
            auto [name, ptr] = wrapper();
            if (!functionRegistry->IsLoadedUdfModule(name)) {
                functionRegistry->AddModule(TString(NKikimr::NMiniKQL::StaticModulePrefix) + name, name, std::move(ptr));
            }
        }
    } else {
        NKikimr::NMiniKQL::FillStaticModules(*functionRegistry);
    }

    return functionRegistry;
}


class TMain : public TMainClassArgs {
    inline static const TString YqlToken = GetEnv(NKqpRun::YQL_TOKEN_VARIABLE);
    inline static std::vector<std::unique_ptr<TFileOutput>> FileHolders;

    TExecutionOptions ExecutionOptions;
    NKqpRun::TRunnerOptions RunnerOptions;

    THashMap<TString, TString> TablesMapping;
    TVector<TString> UdfsPaths;
    TString UdfsDirectory;
    bool ExcludeLinkedUdfs = false;
    bool EmulateYt = false;

    static TString LoadFile(const TString& file) {
        return TFileInput(file).ReadAll();
    }

    static IOutputStream* GetDefaultOutput(const TString& file) {
        if (file == "-") {
            return &Cout;
        }
        if (file) {
            FileHolders.emplace_back(new TFileOutput(file));
            return FileHolders.back().get();
        }
        return nullptr;
    }

    template <typename TResult>
    class TChoices {
    public:
        explicit TChoices(std::map<TString, TResult> choicesMap)
            : ChoicesMap(std::move(choicesMap))
        {}

        TResult operator()(const TString& choice) const {
            return ChoicesMap.at(choice);
        }

        TVector<TString> GetChoices() const {
            TVector<TString> choices;
            choices.reserve(ChoicesMap.size());
            for (const auto& [choice, _] : ChoicesMap) {
                choices.emplace_back(choice);
            }
            return choices;
        }

    private:
        const std::map<TString, TResult> ChoicesMap;
    };

protected:
    void RegisterOptions(NLastGetopt::TOpts& options) override {
        options.SetTitle("KqpRun -- tool to execute queries by using kikimr provider (instead of dq provider in DQrun tool)");
        options.AddHelpOption('h');
        options.SetFreeArgsNum(0);

        // Inputs

        options.AddLongOption('s', "scheme-query", "Scheme query to execute (typically DDL/DCL query)")
            .RequiredArgument("file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                ExecutionOptions.SchemeQuery = LoadFile(option->CurVal());
            });
        options.AddLongOption('p', "script-query", "Script query to execute (typically DML query)")
            .RequiredArgument("file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                ExecutionOptions.ScriptQueries.emplace_back(LoadFile(option->CurVal()));
            });
        options.AddLongOption("templates", "Enable templates for -s and -p queries, such as ${YQL_TOKEN} and ${QUERY_ID}")
            .NoArgument()
            .SetFlag(&ExecutionOptions.UseTemplates);

        options.AddLongOption('t', "table", "File with input table (can be used by YT with -E flag), table@file")
            .RequiredArgument("table@file")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                TStringBuf tableName;
                TStringBuf filePath;
                TStringBuf(option->CurVal()).Split('@', tableName, filePath);
                if (tableName.empty() || filePath.empty()) {
                    ythrow yexception() << "Incorrect table mapping, expected form table@file, e.g. yt.Root/plato.Input@input.txt";
                }
                if (TablesMapping.contains(tableName)) {
                    ythrow yexception() << "Got duplicate table name: " << tableName;
                }
                TablesMapping[tableName] = filePath;
            });

        options.AddLongOption('c', "app-config", "File with app config (TAppConfig for ydb tenant)")
            .RequiredArgument("file")
            .DefaultValue("./configuration/app_config.conf")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                TString file(option->CurValOrDef());
                if (file.EndsWith(".yaml")) {
                    auto document = NKikimr::NFyaml::TDocument::Parse(LoadFile(file));
                    RunnerOptions.YdbSettings.AppConfig = NKikimr::NYamlConfig::YamlToProto(document.Root());
                } else if (!google::protobuf::TextFormat::ParseFromString(LoadFile(file), &RunnerOptions.YdbSettings.AppConfig)) {
                    ythrow yexception() << "Bad format of app configuration";
                }
            });

        options.AddLongOption('u', "udf", "Load shared library with UDF by given path")
            .RequiredArgument("file")
            .EmplaceTo(&UdfsPaths);
        options.AddLongOption("udfs-dir", "Load all shared libraries with UDFs found in given directory")
            .RequiredArgument("directory")
            .StoreResult(&UdfsDirectory);
        options.AddLongOption("exclude-linked-udfs", "Exclude linked udfs when same udf passed from -u or --udfs-dir")
            .NoArgument()
            .SetFlag(&ExcludeLinkedUdfs);

        // Outputs

        options.AddLongOption("log-file", "File with execution logs (writes in stderr if empty)")
            .RequiredArgument("file")
            .StoreResult(&RunnerOptions.YdbSettings.LogOutputFile)
            .Handler1([](const NLastGetopt::TOptsParser* option) {
                if (const TString& file = option->CurVal()) {
                    std::remove(file.c_str());
                }
            });
        TChoices<NKqpRun::TRunnerOptions::ETraceOptType> traceOpt({
            {"all", NKqpRun::TRunnerOptions::ETraceOptType::All},
            {"scheme", NKqpRun::TRunnerOptions::ETraceOptType::Scheme},
            {"script", NKqpRun::TRunnerOptions::ETraceOptType::Script},
            {"disabled", NKqpRun::TRunnerOptions::ETraceOptType::Disabled}
        });
        options.AddLongOption('T', "trace-opt", "print AST in the begin of each transformation")
            .RequiredArgument("trace-opt-query")
            .DefaultValue("disabled")
            .Choices(traceOpt.GetChoices())
            .StoreMappedResultT<TString>(&RunnerOptions.TraceOptType, [this, traceOpt](const TString& choise) {
                auto traceOptType = traceOpt(choise);
                RunnerOptions.YdbSettings.TraceOptEnabled = traceOptType != NKqpRun::TRunnerOptions::ETraceOptType::Disabled;
                return traceOptType;
            });
        options.AddLongOption("trace-id", "Trace id for -p queries")
            .RequiredArgument("id")
            .EmplaceTo(&ExecutionOptions.TraceIds);

        options.AddLongOption("result-file", "File with script execution results (use '-' to write in stdout)")
            .RequiredArgument("file")
            .DefaultValue("-")
            .StoreMappedResultT<TString>(&RunnerOptions.ResultOutput, &GetDefaultOutput);
        options.AddLongOption('L', "result-rows-limit", "Rows limit for script execution results")
            .RequiredArgument("uint")
            .DefaultValue(0)
            .StoreResult(&ExecutionOptions.ResultsRowsLimit);
        TChoices<NKqpRun::TRunnerOptions::EResultOutputFormat> resultFormat({
            {"rows", NKqpRun::TRunnerOptions::EResultOutputFormat::RowsJson},
            {"full-json", NKqpRun::TRunnerOptions::EResultOutputFormat::FullJson},
            {"full-proto", NKqpRun::TRunnerOptions::EResultOutputFormat::FullProto}
        });
        options.AddLongOption('R', "result-format", "Script query result format")
            .RequiredArgument("result-format")
            .DefaultValue("rows")
            .Choices(resultFormat.GetChoices())
            .StoreMappedResultT<TString>(&RunnerOptions.ResultOutputFormat, resultFormat);

        options.AddLongOption("scheme-ast-file", "File with scheme query ast (use '-' to write in stdout)")
            .RequiredArgument("file")
            .StoreMappedResultT<TString>(&RunnerOptions.SchemeQueryAstOutput, &GetDefaultOutput);

        options.AddLongOption("script-ast-file", "File with script query ast (use '-' to write in stdout)")
            .RequiredArgument("file")
            .StoreMappedResultT<TString>(&RunnerOptions.ScriptQueryAstOutput, &GetDefaultOutput);

        options.AddLongOption("script-plan-file", "File with script query plan (use '-' to write in stdout)")
            .RequiredArgument("file")
            .StoreMappedResultT<TString>(&RunnerOptions.ScriptQueryPlanOutput, &GetDefaultOutput);
        options.AddLongOption("script-statistics", "File with script inprogress statistics")
            .RequiredArgument("file")
            .StoreMappedResultT<TString>(&RunnerOptions.InProgressStatisticsOutputFile, [](const TString& file) {
                if (file == "-") {
                    ythrow yexception() << "Script in progress statistics cannot be printed to stdout, please specify file name";
                }
                return file;
            });
        TChoices<NYdb::NConsoleClient::EDataFormat> planFormat({
            {"pretty", NYdb::NConsoleClient::EDataFormat::Pretty},
            {"table", NYdb::NConsoleClient::EDataFormat::PrettyTable},
            {"json", NYdb::NConsoleClient::EDataFormat::JsonUnicode},
        });
        options.AddLongOption('P', "plan-format", "Script query plan format")
            .RequiredArgument("plan-format")
            .DefaultValue("pretty")
            .Choices(planFormat.GetChoices())
            .StoreMappedResultT<TString>(&RunnerOptions.PlanOutputFormat, planFormat);

        options.AddLongOption("script-timeline-file", "File with script query timline in svg format")
            .RequiredArgument("file")
            .StoreMappedResultT<TString>(&RunnerOptions.ScriptQueryTimelineFile, [](const TString& file) {
                if (file == "-") {
                    ythrow yexception() << "Script timline cannot be printed to stdout, please specify file name";
                }
                return file;
            });

        // Pipeline settings

        TChoices<TExecutionOptions::EExecutionCase> executionCase({
            {"script", TExecutionOptions::EExecutionCase::GenericScript},
            {"query", TExecutionOptions::EExecutionCase::GenericQuery},
            {"yql-script", TExecutionOptions::EExecutionCase::YqlScript},
            {"async", TExecutionOptions::EExecutionCase::AsyncQuery}
        });
        options.AddLongOption('C', "execution-case", "Type of query for -p argument")
            .RequiredArgument("query-type")
            .Choices(executionCase.GetChoices())
            .Handler1([this, executionCase](const NLastGetopt::TOptsParser* option) {
                TString choice(option->CurValOrDef());
                ExecutionOptions.ExecutionCases.emplace_back(executionCase(choice));
            });
        options.AddLongOption("inflight-limit", "In flight limit for async queries (use 0 for unlimited)")
            .RequiredArgument("uint")
            .DefaultValue(0)
            .StoreResult(&RunnerOptions.YdbSettings.AsyncQueriesSettings.InFlightLimit);
        TChoices<NKqpRun::TAsyncQueriesSettings::EVerbose> verbose({
            {"each-query", NKqpRun::TAsyncQueriesSettings::EVerbose::EachQuery},
            {"final", NKqpRun::TAsyncQueriesSettings::EVerbose::Final}
        });
        options.AddLongOption("async-verbose", "Verbose type for async queries")
            .RequiredArgument("type")
            .DefaultValue("each-query")
            .Choices(verbose.GetChoices())
            .StoreMappedResultT<TString>(&RunnerOptions.YdbSettings.AsyncQueriesSettings.Verbose, verbose);

        TChoices<NKikimrKqp::EQueryAction> scriptAction({
            {"execute", NKikimrKqp::QUERY_ACTION_EXECUTE},
            {"explain", NKikimrKqp::QUERY_ACTION_EXPLAIN}
        });
        options.AddLongOption('A', "script-action", "Script query execute action")
            .RequiredArgument("script-action")
            .Choices(scriptAction.GetChoices())
            .Handler1([this, scriptAction](const NLastGetopt::TOptsParser* option) {
                TString choice(option->CurValOrDef());
                ExecutionOptions.ScriptQueryActions.emplace_back(scriptAction(choice));
            });

        options.AddLongOption("timeout", "Timeout in milliseconds for -p queries")
            .RequiredArgument("uint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                ExecutionOptions.Timeouts.emplace_back(TDuration::MilliSeconds<ui64>(FromString(option->CurValOrDef())));
            });

        options.AddLongOption("cancel-after", "Cancel script execution operation after specified delay in milliseconds")
            .RequiredArgument("uint")
            .StoreMappedResultT<ui64>(&RunnerOptions.ScriptCancelAfter, &TDuration::MilliSeconds<ui64>);

        options.AddLongOption('F', "forget", "Forget script execution operation after fetching results")
            .NoArgument()
            .SetFlag(&ExecutionOptions.ForgetExecution);

        options.AddLongOption("loop-count", "Number of runs of the script query (use 0 to start infinite loop)")
            .RequiredArgument("uint")
            .DefaultValue(ExecutionOptions.LoopCount)
            .StoreResult(&ExecutionOptions.LoopCount);
        options.AddLongOption("loop-delay", "Delay in milliseconds between loop steps")
            .RequiredArgument("uint")
            .DefaultValue(0)
            .StoreMappedResultT<ui64>(&ExecutionOptions.LoopDelay, &TDuration::MilliSeconds<ui64>);
        options.AddLongOption("continue-after-fail", "Don't not stop requests execution after fails")
            .NoArgument()
            .SetFlag(&ExecutionOptions.ContinueAfterFail);

        options.AddLongOption('D', "database", "Database path for -p queries")
            .RequiredArgument("path")
            .EmplaceTo(&ExecutionOptions.Databases);

        options.AddLongOption('U', "user", "User SID for -p queries")
            .RequiredArgument("user-SID")
            .EmplaceTo(&ExecutionOptions.UserSIDs);

        options.AddLongOption("pool", "Workload manager pool in which queries will be executed")
            .RequiredArgument("pool-id")
            .EmplaceTo(&ExecutionOptions.PoolIds);

        options.AddLongOption("same-session", "Run all -p requests in one session")
            .NoArgument()
            .SetFlag(&RunnerOptions.YdbSettings.SameSession);

        // Cluster settings

        options.AddLongOption('N', "node-count", "Number of nodes to create")
            .RequiredArgument("uint")
            .DefaultValue(RunnerOptions.YdbSettings.NodeCount)
            .StoreMappedResultT<ui32>(&RunnerOptions.YdbSettings.NodeCount, [](ui32 nodeCount) {
                if (nodeCount < 1) {
                    ythrow yexception() << "Number of nodes less than one";
                }
                return nodeCount;
            });

        options.AddLongOption('M', "monitoring", "Embedded UI port (use 0 to start on random free port), if used kqprun will be run as daemon")
            .RequiredArgument("uint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                if (const TString& port = option->CurVal()) {
                    RunnerOptions.YdbSettings.MonitoringEnabled = true;
                    RunnerOptions.YdbSettings.MonitoringPortOffset = FromString(port);
                }
            });

        options.AddLongOption('G', "grpc", "gRPC port (use 0 to start on random free port), if used kqprun will be run as daemon")
            .RequiredArgument("uint")
            .Handler1([this](const NLastGetopt::TOptsParser* option) {
                if (const TString& port = option->CurVal()) {
                    RunnerOptions.YdbSettings.GrpcEnabled = true;
                    RunnerOptions.YdbSettings.GrpcPort = FromString(port);
                }
            });

        options.AddLongOption('E', "emulate-yt", "Emulate YT tables (use file gateway instead of native gateway)")
            .NoArgument()
            .SetFlag(&EmulateYt);

        options.AddLongOption("domain", "Test cluster domain name")
            .RequiredArgument("name")
            .DefaultValue(RunnerOptions.YdbSettings.DomainName)
            .StoreResult(&RunnerOptions.YdbSettings.DomainName);

        options.AddLongOption("dedicated", "Dedicated tenant path, relative inside domain")
            .RequiredArgument("path")
            .InsertTo(&RunnerOptions.YdbSettings.DedicatedTenants);

        options.AddLongOption("shared", "Shared tenant path, relative inside domain")
            .RequiredArgument("path")
            .InsertTo(&RunnerOptions.YdbSettings.SharedTenants);

        options.AddLongOption("serverless", "Serverless tenant path, relative inside domain (use string serverless-name@shared-name to specify shared database)")
            .RequiredArgument("path")
            .InsertTo(&RunnerOptions.YdbSettings.ServerlessTenants);

        options.AddLongOption("storage-size", "Domain storage size in gigabytes")
            .RequiredArgument("uint")
            .DefaultValue(32)
            .StoreMappedResultT<ui32>(&RunnerOptions.YdbSettings.DiskSize, [](ui32 diskSize) {
                return static_cast<ui64>(diskSize) << 30;
            });

        options.AddLongOption("real-pdisks", "Use real PDisks instead of in memory PDisks (also disable disk mock)")
            .NoArgument()
            .SetFlag(&RunnerOptions.YdbSettings.UseRealPDisks);

        options.AddLongOption("disable-disk-mock", "Disable disk mock on single node cluster")
            .NoArgument()
            .SetFlag(&RunnerOptions.YdbSettings.DisableDiskMock);

        TChoices<std::function<void()>> backtrace({
            {"heavy", &NKikimr::EnableYDBBacktraceFormat},
            {"light", []() { SetFormatBackTraceFn(FormatBackTrace); }}
        });
        options.AddLongOption("backtrace", "Default backtrace format function")
            .RequiredArgument("backtrace-type")
            .DefaultValue("heavy")
            .Choices(backtrace.GetChoices())
            .Handler1([backtrace](const NLastGetopt::TOptsParser* option) {
                TString choice(option->CurValOrDef());
                backtrace(choice)();
            });
    }

    int DoRun(NLastGetopt::TOptsParseResult&&) override {
        ExecutionOptions.Validate(RunnerOptions);

        if (RunnerOptions.YdbSettings.DisableDiskMock && RunnerOptions.YdbSettings.NodeCount + RunnerOptions.YdbSettings.SharedTenants.size() + RunnerOptions.YdbSettings.DedicatedTenants.size() > 1) {
            ythrow yexception() << "Disable disk mock cannot be used for multi node clusters";
        }

        RunnerOptions.YdbSettings.YqlToken = YqlToken;
        RunnerOptions.YdbSettings.FunctionRegistry = CreateFunctionRegistry(UdfsDirectory, UdfsPaths, ExcludeLinkedUdfs).Get();
        if (ExecutionOptions.ResultsRowsLimit) {
            RunnerOptions.YdbSettings.AppConfig.MutableQueryServiceConfig()->SetScriptResultRowsLimit(ExecutionOptions.ResultsRowsLimit);
        }

        if (EmulateYt) {
            const auto& fileStorageConfig = RunnerOptions.YdbSettings.AppConfig.GetQueryServiceConfig().GetFileStorage();
            auto fileStorage = WithAsync(CreateFileStorage(fileStorageConfig, {MakeYtDownloader(fileStorageConfig)}));
            auto ytFileServices = NYql::NFile::TYtFileServices::Make(RunnerOptions.YdbSettings.FunctionRegistry.Get(), TablesMapping, fileStorage);
            RunnerOptions.YdbSettings.YtGateway = NYql::CreateYtFileGateway(ytFileServices);
            RunnerOptions.YdbSettings.ComputationFactory = NYql::NFile::GetYtFileFactory(ytFileServices);
        } else if (!TablesMapping.empty()) {
            ythrow yexception() << "Tables mapping is not supported without emulate YT mode";
        }

        RunScript(ExecutionOptions, RunnerOptions);
        return 0;
    }
};


void KqprunTerminateHandler() {
    NColorizer::TColors colors = NColorizer::AutoColors(Cerr);

    Cerr << colors.Red() << "======= terminate() call stack ========" << colors.Default() << Endl;
    FormatBackTrace(&Cerr);
    Cerr << colors.Red() << "=======================================" << colors.Default() << Endl;

    abort();
}


void SegmentationFaultHandler(int) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cerr);

    Cerr << colors.Red() << "======= segmentation fault call stack ========" << colors.Default() << Endl;
    FormatBackTrace(&Cerr);
    Cerr << colors.Red() << "==============================================" << colors.Default() << Endl;

    abort();
}


int main(int argc, const char* argv[]) {
    std::set_terminate(KqprunTerminateHandler);
    signal(SIGSEGV, &SegmentationFaultHandler);

    try {
        TMain().Run(argc, argv);
    } catch (...) {
        NColorizer::TColors colors = NColorizer::AutoColors(Cerr);

        Cerr << colors.Red() <<  CurrentExceptionMessage() << colors.Default() << Endl;
        return 1;
    }

    return 0;
}
