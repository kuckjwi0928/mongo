/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace cluster_parameters {

void validateParameter(OperationContext* opCtx,
                       BSONObj doc,
                       const boost::optional<TenantId>& tenantId);

void updateParameter(OperationContext* opCtx,
                     BSONObj doc,
                     StringData mode,
                     const boost::optional<TenantId>& tenantId);

void clearParameter(OperationContext* opCtx,
                    ServerParameter* sp,
                    const boost::optional<TenantId>& tenantId);

void clearParameter(OperationContext* opCtx,
                    StringData id,
                    const boost::optional<TenantId>& tenantId);

void clearAllTenantParameters(OperationContext* opCtx, const boost::optional<TenantId>& tenantId);

/**
 * Used to initialize in-memory cluster parameter state based on the on-disk contents after startup
 * recovery or initial sync is complete.
 */
void initializeAllTenantParametersFromCollection(OperationContext* opCtx, const Collection& coll);

/**
 * Used on rollback. Updates settings which are present and clears settings which are not.
 */
void resynchronizeAllTenantParametersFromCollection(OperationContext* opCtx,
                                                    const Collection& coll);

template <typename OnEntry>
void doLoadAllTenantParametersFromCollection(OperationContext* opCtx,
                                             const Collection& coll,
                                             StringData mode,
                                             OnEntry onEntry) try {
    invariant(coll.ns() == NamespaceString::makeClusterParametersNSS(coll.ns().tenantId()));

    // If the RecoveryUnit already had an open snapshot, keep the snapshot open. Otherwise
    // abandon the snapshot when exiting the function.
    ScopeGuard scopeGuard([&] { opCtx->recoveryUnit()->abandonSnapshot(); });
    if (opCtx->recoveryUnit()->isActive()) {
        scopeGuard.dismiss();
    }

    std::vector<Status> failures;

    auto cursor = coll.getCursor(opCtx);
    for (auto doc = cursor->next(); doc; doc = cursor->next()) {
        try {
            auto data = doc.get().data.toBson();
            validateParameter(opCtx, data, coll.ns().tenantId());
            onEntry(opCtx, data, mode, coll.ns().tenantId());
        } catch (const DBException& ex) {
            failures.push_back(ex.toStatus());
        }
    }

    if (!failures.empty()) {
        StringBuilder msg;
        for (const auto& failure : failures) {
            msg << failure.toString() << ", ";
        }
        msg.reset(msg.len() - 2);
        uasserted(ErrorCodes::OperationFailed, msg.str());
    }
} catch (const DBException& ex) {
    uassertStatusOK(ex.toStatus().withContext(
        str::stream() << "Failed " << mode << " cluster server parameters from disk"));
}

}  // namespace cluster_parameters

}  // namespace mongo
