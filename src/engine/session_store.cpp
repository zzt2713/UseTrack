#include "engine/session_store.h"

namespace patina::engine
{

ScopedStoreTransaction::ScopedStoreTransaction(SessionStore& store) : _store(store)
{
    _active = _store.begin().ok();
}

ScopedStoreTransaction::~ScopedStoreTransaction()
{
    if (_active) {
        _store.rollback();
    }
}

data::Status ScopedStoreTransaction::commit()
{
    if (!_active) {
        return data::Status::failure(QStringLiteral("事务未开始"));
    }
    const data::Status status = _store.commit();
    // 无论成功与否都不再回滚：提交成功再回滚是错的，提交失败时 SQLite
    // 已经把事务结束了。
    _active = false;
    return status;
}

} // namespace patina::engine
