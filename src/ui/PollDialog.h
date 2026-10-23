#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// Create a Poll, from the composer: a question, up to ten answers, how long
// it stays open, and whether more than one answer may be chosen. The limits
// are Discord's own: 300 characters of question, 55 an answer.
class PollDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PollDialog(QWidget *parent = nullptr);

    // What goes out as the message's `poll`.
    QJsonObject poll() const;

    // The same from plain values, which is what tools/test_polls checks.
    // Blank answers are left out.
    static QJsonObject pollBody(const QString &question, const QStringList &answers, int hours, bool multiselect);

    static constexpr int MaxAnswers = 10;
    static constexpr int MaxQuestion = 300;
    static constexpr int MaxAnswer = 55;

private:
    void addAnswer();
    void updatePost();
    QStringList answers() const;

    QLineEdit *m_question = nullptr;
    QList<QLineEdit *> m_answers;
    QVBoxLayout *m_answerLayout = nullptr;
    QPushButton *m_addAnswer = nullptr;
    QComboBox *m_duration = nullptr;
    QCheckBox *m_multiselect = nullptr;
    QPushButton *m_post = nullptr;
};
