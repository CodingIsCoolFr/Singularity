#include "ui/PollDialog.h"

#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPair>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

PollDialog::PollDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Create a Poll"));
    resize(440, 0);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    const auto heading = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
                                 .arg(QLatin1String(Theme::TextMuted)));
        return label;
    };

    layout->addWidget(heading(QStringLiteral("Question")));
    m_question = new QLineEdit(this);
    m_question->setPlaceholderText(QStringLiteral("What question do you want to ask?"));
    m_question->setMaxLength(MaxQuestion);
    connect(m_question, &QLineEdit::textChanged, this, &PollDialog::updatePost);
    layout->addWidget(m_question);

    layout->addSpacing(6);
    layout->addWidget(heading(QStringLiteral("Answers")));
    m_answerLayout = new QVBoxLayout;
    m_answerLayout->setSpacing(6);
    layout->addLayout(m_answerLayout);

    m_addAnswer = new QPushButton(QStringLiteral("Add another answer"), this);
    connect(m_addAnswer, &QPushButton::clicked, this, [this]() {
        addAnswer();
        m_answers.last()->setFocus();
    });
    layout->addWidget(m_addAnswer, 0, Qt::AlignLeft);

    // Discord's choices, in hours; a day is what it starts on.
    layout->addSpacing(6);
    auto *options = new QHBoxLayout;
    options->addWidget(heading(QStringLiteral("Duration")));
    m_duration = new QComboBox(this);
    const QList<QPair<QString, int>> durations = {
        {QStringLiteral("1 hour"), 1},   {QStringLiteral("4 hours"), 4},  {QStringLiteral("8 hours"), 8},
        {QStringLiteral("24 hours"), 24}, {QStringLiteral("3 days"), 72}, {QStringLiteral("1 week"), 168},
        {QStringLiteral("2 weeks"), 336},
    };
    for (const QPair<QString, int> &duration : durations)
        m_duration->addItem(duration.first, duration.second);
    m_duration->setCurrentIndex(3);
    options->addWidget(m_duration);
    options->addStretch(1);
    m_multiselect = new QCheckBox(QStringLiteral("Allow multiple answers"), this);
    options->addWidget(m_multiselect);
    layout->addLayout(options);

    layout->addSpacing(8);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *cancel = new QPushButton(QStringLiteral("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_post = new QPushButton(QStringLiteral("Post"), this);
    m_post->setObjectName(QStringLiteral("PrimaryButton"));
    m_post->setDefault(true);
    connect(m_post, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(m_post);
    layout->addLayout(buttons);

    addAnswer();
    addAnswer();
    updatePost();
    m_question->setFocus();
}

void PollDialog::addAnswer()
{
    if (m_answers.size() >= MaxAnswers)
        return;
    auto *answer = new QLineEdit(this);
    answer->setPlaceholderText(QStringLiteral("Type your answer"));
    answer->setMaxLength(MaxAnswer);
    connect(answer, &QLineEdit::textChanged, this, &PollDialog::updatePost);
    m_answerLayout->addWidget(answer);
    m_answers.append(answer);
    m_addAnswer->setVisible(m_answers.size() < MaxAnswers);
}

QStringList PollDialog::answers() const
{
    QStringList texts;
    for (const QLineEdit *answer : m_answers)
        texts.append(answer->text());
    return texts;
}

// A question and one answer at least, as Discord requires.
void PollDialog::updatePost()
{
    bool answered = false;
    for (const QLineEdit *answer : std::as_const(m_answers))
        answered = answered || !answer->text().trimmed().isEmpty();
    m_post->setEnabled(answered && !m_question->text().trimmed().isEmpty());
}

QJsonObject PollDialog::poll() const
{
    return pollBody(m_question->text(), answers(), m_duration->currentData().toInt(), m_multiselect->isChecked());
}

QJsonObject PollDialog::pollBody(const QString &question, const QStringList &answers, int hours, bool multiselect)
{
    QJsonArray list;
    for (const QString &answer : answers) {
        const QString text = answer.trimmed().left(MaxAnswer);
        if (!text.isEmpty() && list.size() < MaxAnswers)
            list.append(QJsonObject{{QStringLiteral("poll_media"), QJsonObject{{QStringLiteral("text"), text}}}});
    }
    return QJsonObject{
        {QStringLiteral("question"), QJsonObject{{QStringLiteral("text"), question.trimmed().left(MaxQuestion)}}},
        {QStringLiteral("answers"), list},
        {QStringLiteral("allow_multiselect"), multiselect},
        {QStringLiteral("duration"), hours},
        {QStringLiteral("layout_type"), 1},
    };
}
