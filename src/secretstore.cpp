#include "secretstore.h"
#include <QByteArray>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

QString SecretStore::protect(const QString &plain, QString *error)
{
    if (plain.isEmpty()) return {};
#ifdef Q_OS_WIN
    auto bytes = plain.toUtf8();
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Yiyi API credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (error) *error = QStringLiteral("无法使用 Windows 加密凭据，请检查当前用户环境。");
        return {};
    }
    const auto encoded = QByteArray(reinterpret_cast<char *>(out.pbData), out.cbData).toBase64();
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    bytes.fill('\0');
    return QStringLiteral("dpapi:") + QString::fromLatin1(encoded);
#else
    if (error) *error = QStringLiteral("当前版本的持久化凭据保护仅支持 Windows。");
    return {};
#endif
}

QString SecretStore::reveal(const QString &encoded, QString *error)
{
    if (encoded.isEmpty()) return {};
#ifdef Q_OS_WIN
    if (!encoded.startsWith(QLatin1String("dpapi:"))) {
        if (error) *error = QStringLiteral("凭据格式无效，请重新填写 API Key。");
        return {};
    }
    auto bytes = QByteArray::fromBase64(encoded.mid(6).toLatin1());
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (error) *error = QStringLiteral("无法解密 API Key，请在当前 Windows 账户下重新填写。");
        return {};
    }
    const QString result = QString::fromUtf8(reinterpret_cast<char *>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
#else
    if (error) *error = QStringLiteral("当前平台无法解密此凭据。");
    return {};
#endif
}
