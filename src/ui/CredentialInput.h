#pragma once

class QLabel;
class QLineEdit;

namespace noxshell::ui {

// UI input policy only. Never apply this to stored credentials or SSH payloads.
void configureAsciiCredentialInput(QLineEdit *editor, QLabel *feedback);

} // namespace noxshell::ui
