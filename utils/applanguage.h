#pragma once

#include <QCoreApplication>
#include <QString>

class AppText {
    Q_DECLARE_TR_FUNCTIONS(AppText)
public:
    // Translate presentation-only status strings without changing stored state.
    static QString display(const QString &source);
};

namespace AppLanguage {
QString normalized(QString language);
QString current();
bool initialize(QCoreApplication &application, const QString &language);
}
