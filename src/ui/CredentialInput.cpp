#include "CredentialInput.h"

#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QValidator>

namespace noxshell::ui {
namespace {

class AsciiCredentialValidator final : public QValidator {
public:
    AsciiCredentialValidator(QLineEdit *editor, QLabel *feedback)
        : QValidator(editor), m_feedback(feedback) {}

    State validate(QString &input, int &position) const override
    {
        QString normalized;
        normalized.reserve(input.size());
        int cursor = 0;
        for (qsizetype index = 0; index < input.size(); ++index) {
            const auto code = input.at(index).unicode();
            if (code >= 0x20 && code <= 0x7e) normalized.append(input.at(index));
            else if (code >= 0xff01 && code <= 0xff5e) normalized.append(QChar(code - 0xfee0));
            else if (code == 0x3002 || code == 0xff61) normalized.append(QLatin1Char('.'));
            else if (code == 0x3001) normalized.append(QLatin1Char(','));
            else if (code == 0x3000 || code == 0x00a0) normalized.append(QLatin1Char(' '));
            else if (code == 0x2018 || code == 0x2019) normalized.append(QLatin1Char('\''));
            else if (code == 0x201c || code == 0x201d) normalized.append(QLatin1Char('"'));
            else if (code == 0x2013 || code == 0x2014) normalized.append(QLatin1Char('-'));
            else if (code == 0x2026) normalized.append(QStringLiteral("..."));
            else if (code == 0x3010) normalized.append(QLatin1Char('['));
            else if (code == 0x3011) normalized.append(QLatin1Char(']'));
            else {
                // Reject the edit as a whole instead of silently deleting part
                // of a paste or erasing the current selection for a Chinese key.
                if (m_feedback) m_feedback->setText(QStringLiteral(
                    "含中文或非英文字符，本次输入未写入；请使用英文半角。"));
                return Invalid;
            }
            if (index < position) cursor = normalized.size();
        }
        if (normalized != input) {
            input = normalized;
            position = cursor;
            if (m_feedback) m_feedback->setText(QStringLiteral(
                "已将中文/全角标点转换为英文半角，请核对当前输入。"));
        }
        return Acceptable;
    }

private:
    QPointer<QLabel> m_feedback;
};

} // namespace

void configureAsciiCredentialInput(QLineEdit *editor, QLabel *feedback)
{
    editor->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData
        | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhLatinOnly);
    editor->setValidator(new AsciiCredentialValidator(editor, feedback));
    feedback->setObjectName(QStringLiteral("asciiCredentialHint"));
    feedback->setTextFormat(Qt::PlainText);
    feedback->setWordWrap(true);
    feedback->setText(QStringLiteral("仅英文半角；中文/全角标点自动转换，不接受汉字。"));
    QObject::connect(editor, &QLineEdit::textChanged, feedback, [feedback](const QString &text) {
        if (text.isEmpty()) {
            feedback->setText(QStringLiteral("仅英文半角；中文/全角标点自动转换，不接受汉字。"));
        }
    });
}

} // namespace noxshell::ui
