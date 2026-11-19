#pragma once
#include <QString>

namespace SecretStore {
QString protect(const QString &plain, QString *error = nullptr);
QString reveal(const QString &encoded, QString *error = nullptr);
}
