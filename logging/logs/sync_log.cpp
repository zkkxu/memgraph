#include "sync_log.hpp"

void SyncLog::emit(Record::uptr record)
{
    auto guard = this->acquire_unique();
    dispatch(*record);
}
