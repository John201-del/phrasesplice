/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QObject>
#include <QJsonObject>
#include <QStringList>
#include <memory>

class AssistantPanel;

class AssistantController : public QObject
{
    Q_OBJECT
public:
    explicit AssistantController(AssistantPanel *panel, QObject *parent = nullptr);
    ~AssistantController() override;
    void beginConnection();

private:
    friend class AssistantControllerTest;
    static bool acceptsImportedAssetLoad(const QJsonObject &before, const QJsonObject &after, const QStringList &ids);
    struct State;
    std::unique_ptr<State> d;
};
