IF (NOT OPENSOURCE)

UNITTEST()

SRCS(
    yql_yt_native_folders_ut.cpp
)

PEERDIR(
    ydb/library/yql/providers/yt/gateway/native
    ydb/library/yql/providers/yt/gateway/file
    ydb/library/yql/providers/yt/codec/codegen
    ydb/library/yql/providers/yt/comp_nodes/llvm14
    yql/essentials/core/ut_common
    library/cpp/testing/mock_server
    library/cpp/testing/common
    yql/essentials/public/udf/service/terminate_policy
    yql/essentials/sql/pg
    yql/essentials/minikql/comp_nodes/llvm14
    yql/essentials/minikql/invoke_builtins/llvm14
)

YQL_LAST_ABI_VERSION()

END()

ENDIF()

