#include "tagsearchindex.h"

namespace TagSearch {

QString normalizedText(const QString &text)
{
    const QString folded = text.toCaseFolded();
    QString result;
    result.reserve(folded.size());
    bool separator = true;
    for (const QChar ch : folded) {
        if (ch.isSpace() || QStringView(u"_-.,;:!?|/\\()[]{}<>\"").contains(ch)) {
            if (!separator) result.append(' ');
            separator = true;
        } else {
            result.append(ch);
            separator = false;
        }
    }
    if (result.endsWith(' ')) result.chop(1);
    return result;
}

static bool matchesText(const QString &text, const Query &query)
{
    if (query.mode == ExactMatch) return text == query.text;
    if (query.mode == ContainsMatch) return text.contains(query.text);
    qsizetype position = -1;
    while ((position = text.indexOf(query.text, position + 1)) >= 0) {
        const qsizetype end = position + query.text.size();
        if ((position == 0 || text.at(position - 1) == ' ')
            && (end == text.size() || text.at(end) == ' ')) return true;
    }
    return false;
}

bool matches(const Row &row, const Query &query)
{
    return query.text.isEmpty() || matchesText(row.tag, query) || matchesText(row.translation, query);
}

QVector<quint8> matchRows(const QVector<Row> &rows, const Query &query,
                        const std::shared_ptr<std::atomic_bool> &cancelled)
{
    QVector<quint8> result(rows.size());
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if ((i % 256) == 0 && cancelled->load(std::memory_order_relaxed)) return {};
        result[i] = matches(rows.at(i), query);
    }
    return result;
}

}
