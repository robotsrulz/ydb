#include "yql_s3_source_factory.h"

#include <util/system/platform.h>
#if defined(_linux_) || defined(_darwin_)
#include "yql_s3_read_actor.h"

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/Formats/registerFormats.h>
#endif

namespace NYql::NDq {

void RegisterS3ReadActorFactory(
        TDqAsyncIoFactory& factory,
        ISecuredServiceAccountCredentialsFactory::TPtr credentialsFactory,
        IHTTPGateway::TPtr gateway,
        const std::shared_ptr<NYql::NS3::TRetryConfig>&) {
#if defined(_linux_) || defined(_darwin_)
    NDB::registerFormats();
    factory.RegisterSource<NS3::TSource>("S3Source",
        [credentialsFactory, gateway](NS3::TSource&& settings, IDqAsyncIoFactory::TSourceArguments&& args) {
                return CreateS3ReadActor(args.TypeEnv, args.HolderFactory, gateway, std::move(settings), args.InputIndex, args.SecureParams, args.TaskParams, args.ComputeActorId, credentialsFactory);
        });
#else
    Y_UNUSED(factory);
    Y_UNUSED(credentialsFactory);
    Y_UNUSED(gateway);
#endif
}

}
