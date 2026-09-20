#pragma once

#include <QLayout>
#include <QList>

// A layout that places widgets left to right and wraps to the next line when
// it runs out of room. Qt has no built-in one, and the role pills on a profile
// need it.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int spacing = 6);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;

    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

private:
    // Returns the height used. When `apply` is false nothing is moved, which
    // is how heightForWidth measures without side effects.
    int layoutRows(const QRect &rect, bool apply) const;

    QList<QLayoutItem *> m_items;
    int m_spacing = 6;
};
