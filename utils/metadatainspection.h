#pragma once

#include "core/metadatascantypes.h"
#include <QVector>

namespace MetadataInspection {

QVector<MetadataScanItem> scan(QVector<MetadataScanItem> items);
QVector<MetadataHealthIssue> healthCheck(QVector<MetadataScanItem> items);

}
