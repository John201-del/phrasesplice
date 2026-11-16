/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

struct AssistantConnectionOptions
{
    QString provider;
    QString endpoint;
    QString model;
    QString apiKey;
};

class AssistantConnectionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AssistantConnectionDialog(QWidget *parent = nullptr);
    AssistantConnectionOptions options() const;

public Q_SLOTS:
    void accept() override;

private:
    bool usesHttp() const;
    bool hasValidHttpOptions() const;
    void updateProvider();
    void updateActions();

    QComboBox *m_provider;
    QWidget *m_httpFields;
    QLineEdit *m_endpoint;
    QLineEdit *m_model;
    QLineEdit *m_apiKey;
    QLabel *m_help;
    QDialogButtonBox *m_buttons;
    QString m_endpointOrigin;
};
