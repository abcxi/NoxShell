#pragma once

#include <QString>

namespace noxshell {

class AppLogger final {
public:
    static void install();
    [[nodiscard]] static QString logFilePath();
    [[nodiscard]] static QString sanitize(QString message);
};

} // namespace noxshell
