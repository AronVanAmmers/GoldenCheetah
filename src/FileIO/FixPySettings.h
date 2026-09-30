#ifndef FIXPYSETTINGS_H
#define FIXPYSETTINGS_H

#include <QString>
#include <QList>

#include "FixPyScript.h"
#include "FixPyRunner.h"

class FixPySettings
{
public:
    FixPySettings();
    ~FixPySettings();

    void initialize();
    void disableFixPy();
    QList<FixPyScript *> getScripts();
    FixPyScript *getScript(QString name);
    FixPyScript *createScript(QString name);
    void deleteScript(QString name);

    // a file name in the scripts folder for wanted (name.py): as given, or
    // with _1, _2 ... added until no other script and no file has it
    QString uniquePath(const QString &wanted, const FixPyScript *exclude = nullptr) const;

    void save();

private:
    const QString PYFIXES_DIR_NAME = ".pyfixes";
    const QString PYFIXES_SETTINGS_FILE_NAME = "configglobal-pyfixes.ini";

    bool readPyFixFile(QString fixName, QString fixPath, QString iniKey);
    int getMaxKey();

    bool isInitialied;
    QList<FixPyScript *> scripts;
};

extern FixPySettings *fixPySettings;

#endif // FIXPYSETTINGS_H
