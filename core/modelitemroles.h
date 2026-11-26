#ifndef MODELITEMROLES_H
#define MODELITEMROLES_H

#include <Qt>

// 模型列表相关
inline constexpr int ROLE_MODEL_NAME             = Qt::UserRole;
inline constexpr int ROLE_FILE_PATH              = Qt::UserRole + 1;
inline constexpr int ROLE_PREVIEW_PATH           = Qt::UserRole + 2;
inline constexpr int ROLE_NSFW_LEVEL             = Qt::UserRole + 3;
inline constexpr int ROLE_CIVITAI_NAME           = Qt::UserRole + 4;   // 存储从 JSON 读取的真实名称

// 排序与筛选
inline constexpr int ROLE_SORT_DATE              = Qt::UserRole + 10;  // 存储时间戳 (qint64)
inline constexpr int ROLE_SORT_DOWNLOADS         = Qt::UserRole + 11;  // 存储下载量 (int)
inline constexpr int ROLE_SORT_LIKES             = Qt::UserRole + 12;  // 存储点赞量 (int)
inline constexpr int ROLE_FILTER_BASE            = Qt::UserRole + 13;  // 存储底模名称 (QString)
inline constexpr int ROLE_SORT_ADDED             = Qt::UserRole + 14;  // 存储本地文件创建时间 (qint64)
inline constexpr int ROLE_LOCAL_EDITED           = Qt::UserRole + 15;  // 标记本地/已编辑模型 (bool)
inline constexpr int ROLE_SORT_USAGE_COUNT       = Qt::UserRole + 16;  // 本地返图使用次数
inline constexpr int ROLE_SORT_LAST_USED         = Qt::UserRole + 17;  // 本地返图最近使用时间
inline constexpr int ROLE_MODEL_ROOT_PATH        = Qt::UserRole + 18;  // 模型所属的用户配置根目录
inline constexpr int ROLE_MODEL_ROOT_NAME        = Qt::UserRole + 19;  // 模型所属根目录显示名
inline constexpr int ROLE_MODEL_FOLDER_KEY       = Qt::UserRole + 20;  // Models 列表文件夹折叠键
inline constexpr int ROLE_MODEL_FOLDER_COLLAPSED = Qt::UserRole + 21;  // Models 列表文件夹是否折叠
inline constexpr int ROLE_MODEL_FILTER_VISIBLE   = Qt::UserRole + 22;  // 搜索/底模/收藏夹过滤后的可见状态
inline constexpr int ROLE_MODEL_HIGHLIGHT_COLOR  = Qt::UserRole + 23;  // 模型侧边栏高亮色
inline constexpr int ROLE_USER_RATING            = Qt::UserRole + 24;  // 用户评分
inline constexpr int ROLE_USER_NOTE              = Qt::UserRole + 25;  // 用户备注
inline constexpr int ROLE_USER_TAGS              = Qt::UserRole + 26;  // 用户标签
inline constexpr int ROLE_USER_CUSTOM_TRIGGERS   = Qt::UserRole + 27;  // 用户自定义触发词
inline constexpr int ROLE_MODEL_CREATOR          = Qt::UserRole + 28;  // Civitai 作者
inline constexpr int ROLE_MODEL_TAGS             = Qt::UserRole + 29;  // Civitai 模型标签
inline constexpr int ROLE_MODEL_TYPE             = Qt::UserRole + 30;  // Civitai 模型类型，如 LoRA / Checkpoint
inline constexpr int ROLE_MODEL_TRAINED_WORDS    = Qt::UserRole + 31;  // Civitai/metadata 触发词

// 用户图库专用
inline constexpr int ROLE_USER_IMAGE_PATH        = Qt::UserRole + 40;
inline constexpr int ROLE_USER_IMAGE_PROMPT      = Qt::UserRole + 41;
inline constexpr int ROLE_USER_IMAGE_NEG         = Qt::UserRole + 42;
inline constexpr int ROLE_USER_IMAGE_PARAMS      = Qt::UserRole + 43;
inline constexpr int ROLE_USER_IMAGE_TAGS        = Qt::UserRole + 44;
inline constexpr int ROLE_USER_IMAGE_NEG_TAGS    = Qt::UserRole + 45;
inline constexpr int ROLE_EDIT_IMAGE_PATH        = Qt::UserRole + 46;
inline constexpr int ROLE_IS_FOLDER_HEADER       = Qt::UserRole + 47;
inline constexpr int ROLE_USER_IMAGE_TAG_KEYS    = Qt::UserRole + 48;
inline constexpr int ROLE_USER_IMAGE_NEG_KEYS    = Qt::UserRole + 49;
inline constexpr int ROLE_USER_IMAGE_NSFW        = Qt::UserRole + 56;

// 树状图占位符标记
inline constexpr int ROLE_IS_PLACEHOLDER         = Qt::UserRole + 50;
inline constexpr int ROLE_CIVITAI_MODEL_ID       = Qt::UserRole + 51;
inline constexpr int ROLE_CIVITAI_VERSION_ID     = Qt::UserRole + 52;
inline constexpr int ROLE_CIVITAI_SHA256         = Qt::UserRole + 53;
inline constexpr int ROLE_SYNC_FAILED            = Qt::UserRole + 54;
inline constexpr int ROLE_SYNC_ERROR             = Qt::UserRole + 55;

// 收藏夹树状图
inline constexpr int ROLE_IS_COLLECTION_NODE     = Qt::UserRole + 60;  // 标记这是一个收藏夹节点
inline constexpr int ROLE_COLLECTION_NAME        = Qt::UserRole + 61;  // 存储收藏夹名称
inline constexpr int ROLE_ITEM_COUNT             = Qt::UserRole + 62;  // 存储该分类下的模型数量
inline constexpr int ROLE_COLLECTION_EXPAND_KEY  = Qt::UserRole + 63;  // 存储收藏夹树展开状态键
inline constexpr int ROLE_PREVIEW_PLACEHOLDER    = Qt::UserRole + 64;  // 该项当前显示占位图（切主题需重染）
inline constexpr int ROLE_MODEL_PREVIEW_STATE    = Qt::UserRole + 65;  // ModelPreviewState，区分明确无预览与缺失/未知

#endif // MODELITEMROLES_H
