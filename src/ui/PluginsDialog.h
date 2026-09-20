#pragma once

#include <QDialog>

class PluginHost;
class QListWidget;
class QStackedWidget;
class QLabel;

// Lists every built-in plugin, lets you switch each one on or off, and shows
// the settings page a plugin provides.
class PluginsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PluginsDialog(PluginHost *host, QWidget *parent = nullptr);

private:
    void buildList();
    void showPluginAt(int row);

    PluginHost *m_host = nullptr;
    QListWidget *m_list = nullptr;
    QStackedWidget *m_pages = nullptr;
};
