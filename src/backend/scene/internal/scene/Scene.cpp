#include "Scene.h"

#include "fs/VFS.h"
#include "interface/IImageParser.h"
#include "interface/IShaderValueUpdater.h"
#include "particle/ParticleSystem.h"

namespace wallpaper 
{

Scene::Scene(): sceneGraph(std::make_shared<SceneNode>()) ,paritileSys(std::make_unique<ParticleSystem>(*this)) {}
Scene::~Scene() = default;

}


