#ifndef PATINA_DATA_REPOSITORIES_H
#define PATINA_DATA_REPOSITORIES_H

// data/ 的对外入口。
//
// 边界：ui/ 只准 include 本头文件与 data/types.h，
// 不 include 具体 repository 实现以外的 SQL；engine/ 可以 include 具体 repository。
// 任何模块都不准持有 QSqlDatabase 或拼接 SQL —— 那意味着绕过本层。

#include "data/anonymous_activity_repository.h"
#include "data/app_category.h"
#include "data/app_links.h"
#include "data/app_options_read_model.h"
#include "data/app_override.h"
#include "data/app_repository.h"
#include "data/backup_service.h"
#include "data/category_registry.h"
#include "data/classification_repository.h"
#include "data/dashboard_read_model.h"
#include "data/data_read_model.h"
#include "data/database.h"
#include "data/destination_read_model.h"
#include "data/export_service.h"
#include "data/hourly_read_model.h"
#include "data/import_service.h"
#include "data/schema.h"
#include "data/session_repository.h"
#include "data/session_title_sample_repository.h"
#include "data/settings_repository.h"
#include "data/tool_models.h"
#include "data/tools_repository.h"
#include "data/types.h"

#endif // PATINA_DATA_REPOSITORIES_H
