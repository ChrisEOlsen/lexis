#include "ModelLoader.h"

extern "C" {
#include "local_llm_client.h"
}

ModelLoader::ModelLoader(QString modelPath, QObject *parent) : QThread(parent), m_modelPath(std::move(modelPath)) {
}

void ModelLoader::run() {
    int result = local_llm_client_init(m_modelPath.toUtf8().constData());
    emit modelLoadFinished(result == 0);
}
