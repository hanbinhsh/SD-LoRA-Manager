#include "modelfilter.h"

namespace ModelFilter {

QString normalizeModelType(const QString &raw) {
    const QString t = raw.trimmed().toLower();
    if (t.isEmpty()) return QString();
    if (t.contains("lora") || t.contains("locon") || t.contains("dora") || t.contains("lycoris")) return QStringLiteral("LoRA");
    if (t.contains("checkpoint")) return QStringLiteral("Checkpoint");
    if (t.contains("textualinversion") || t.contains("embedding")) return QStringLiteral("Embedding");
    if (t.contains("vae")) return QStringLiteral("VAE");
    if (t.contains("hypernetwork")) return QStringLiteral("Hypernetwork");
    if (t.contains("controlnet")) return QStringLiteral("ControlNet");
    if (t.contains("upscaler")) return QStringLiteral("Upscaler");
    if (t.contains("motion")) return QStringLiteral("MotionModule");
    if (t.contains("poses")) return QStringLiteral("Poses");
    if (t.contains("wildcard")) return QStringLiteral("Wildcards");
    if (t.contains("aestheticgradient")) return QStringLiteral("AestheticGradient");
    return raw.trimmed(); // 其它已知类型按原样展示
}

bool matches(const Record &record, const QString &query,
             const QString &baseModel, const QString &modelType)
{
    if (baseModel != "All" && record.baseModel != baseModel) return false;
    if (modelType != "All" && normalizeModelType(record.modelType) != modelType) return false;
    const QString search = query.trimmed();
    if (search.isEmpty()) return true;
    for (const QString &text : record.searchableText) {
        if (text.contains(search, Qt::CaseInsensitive)) return true;
    }
    return false;
}

}
