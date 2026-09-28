#pragma once

#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

namespace TagSearch {

enum MatchMode { ContainsMatch = 0, WordMatch = 1, ExactMatch = 2 };
struct Row {
    QString tag;
    QString translation;
};
struct Query {
    QString text;
    MatchMode mode = ContainsMatch;
};

// Search punctuation is intentionally broader than prompt deduplication rules.
QString normalizedText(const QString &text);
bool matches(const Row &row, const Query &query);
QVector<quint8> matchRows(const QVector<Row> &rows, const Query &query,
                        const std::shared_ptr<std::atomic_bool> &cancelled);

}
