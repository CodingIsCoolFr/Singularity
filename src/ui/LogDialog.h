#pragma once

#include <QDialog>

class QPlainTextEdit;

// Live view of the app log, plus a button to open the log file.
class LogDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LogDialog(QWidget *parent = nullptr);

private:
    QPlainTextEdit *m_view = nullptr;
};
