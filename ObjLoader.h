#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

// ================================================================
//  Вершина теперь содержит UV-координаты для текстурирования
// ================================================================
struct Vertex
{
    float pos[3];      // POSITION
    float normal[3];   // NORMAL
    float uv[2];       // TEXCOORD  ← новое поле
    float color[4];    // COLOR  (используется если нет текстуры)
};

struct MeshData
{
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
};

// ================================================================
//  Материал — читается из .mtl файла
// ================================================================
struct Material
{
    std::string name;
    std::string diffuseTexture;  // map_Kd
    float Kd[3] = { 0.8f, 0.8f, 0.8f };  // диффузный цвет
    float Ks[3] = { 1.0f, 1.0f, 1.0f };  // зеркальный цвет
    float Ns = 32.0f;                   // shininess
    float d = 1.0f;                    // прозрачность
};

// ================================================================
//  Группа по материалу внутри меша
// ================================================================
struct MeshGroup
{
    MeshData  mesh;
    Material  material;
};

// ----------------------------------------------------------------
//  Парсер MTL
// ----------------------------------------------------------------
inline std::unordered_map<std::string, Material> LoadMTL(const std::string& path)
{
    std::unordered_map<std::string, Material> mats;
    std::ifstream file(path);
    if (!file.is_open()) return mats;

    Material cur;
    std::string line;
    bool hasActive = false;

    auto saveCur = [&]() {
        if (hasActive && !cur.name.empty())
            mats[cur.name] = cur;
        };

    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') continue;
        // trim carriage return
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream ss(line);
        std::string tok;
        ss >> tok;

        if (tok == "newmtl")
        {
            saveCur();
            cur = Material{};
            ss >> cur.name;
            hasActive = true;
        }
        else if (tok == "Kd") { ss >> cur.Kd[0] >> cur.Kd[1] >> cur.Kd[2]; }
        else if (tok == "Ks") { ss >> cur.Ks[0] >> cur.Ks[1] >> cur.Ks[2]; }
        else if (tok == "Ns") { ss >> cur.Ns; }
        else if (tok == "d") { ss >> cur.d; }
        else if (tok == "map_Kd")
        {
            std::string rest;
            std::getline(ss, rest);

            // strip leading/trailing whitespace
            size_t a = rest.find_first_not_of(" \t\r");
            if (a != std::string::npos) rest = rest.substr(a);
            size_t b = rest.find_last_not_of(" \t\r");
            if (b != std::string::npos) rest = rest.substr(0, b + 1);

            // Если путь абсолютный (C:\... или /...) —
            // берём только имя файла и кладём в textures/
            bool isAbsolute = (rest.size() >= 2 && rest[1] == ':') ||
                (!rest.empty() && (rest[0] == '/' || rest[0] == '\\'));
            if (isAbsolute)
            {
                size_t sep = rest.find_last_of("/\\");
                if (sep != std::string::npos)
                    rest = rest.substr(sep + 1);
                rest = "textures/" + rest;
            }

            cur.diffuseTexture = rest;
        }
    }
    saveCur();
    return mats;
}

// ----------------------------------------------------------------
//  Вычислить директорию из пути к файлу
// ----------------------------------------------------------------
inline std::string DirOf(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? "" : path.substr(0, p + 1);
}

// ================================================================
//  LoadOBJ  — поддержка v / vn / vt / f + mtllib / usemtl
//  Возвращает список групп по материалам.
//  Простой вариант: если у модели нет групп, всё идёт в одну.
// ================================================================
inline std::vector<MeshGroup> LoadOBJ(
    const std::string& path,
    float defaultR = 0.8f,
    float defaultG = 0.8f,
    float defaultB = 0.8f)
{
    std::ifstream file(path);
    if (!file.is_open())
        throw std::runtime_error("ObjLoader: cannot open file: " + path);

    std::string dir = DirOf(path);

    struct Vec3 { float x, y, z; };
    struct Vec2 { float u, v; };

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> texcoords;

    std::unordered_map<std::string, Material> allMats;
    std::string activeMat;

    // Пары (vertKey → groupName) для разбивки по материалу
    struct FaceVert { int p = 0, t = -1, n = -1; };

    struct FaceGroup
    {
        std::string matName;
        std::vector<std::vector<FaceVert>> faces;
    };

    std::vector<FaceGroup> groups;
    groups.push_back({ "", {} }); // группа по умолчанию

    auto currentGroup = [&]() -> FaceGroup& {
        return groups.back();
        };

    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string tok;
        ss >> tok;

        if (tok == "v")
        {
            Vec3 v{}; ss >> v.x >> v.y >> v.z; positions.push_back(v);
        }
        else if (tok == "vn")
        {
            Vec3 v{}; ss >> v.x >> v.y >> v.z; normals.push_back(v);
        }
        else if (tok == "vt")
        {
            Vec2 v{}; ss >> v.u >> v.v;
            // OBJ V-ось идёт снизу вверх, DX12 — сверху вниз
            v.v = 1.0f - v.v;
            texcoords.push_back(v);
        }
        else if (tok == "mtllib")
        {
            std::string mtlFile; ss >> mtlFile;
            auto loaded = LoadMTL(dir + mtlFile);
            allMats.insert(loaded.begin(), loaded.end());
        }
        else if (tok == "usemtl")
        {
            std::string matName; ss >> matName;
            if (matName != activeMat)
            {
                activeMat = matName;
                groups.push_back({ activeMat, {} });
            }
        }
        else if (tok == "f")
        {
            std::vector<FaceVert> face;
            std::string ft;
            while (ss >> ft)
            {
                FaceVert fv{};
                // Форматы: p   p/t   p//n   p/t/n
                size_t s1 = ft.find('/');
                fv.p = std::stoi(ft.substr(0, s1)) - 1;

                if (s1 != std::string::npos)
                {
                    size_t s2 = ft.find('/', s1 + 1);
                    // texcoord
                    if (s2 == std::string::npos)
                    {
                        // p/t
                        std::string ts = ft.substr(s1 + 1);
                        if (!ts.empty()) fv.t = std::stoi(ts) - 1;
                    }
                    else
                    {
                        // p/t/n или p//n
                        std::string ts = ft.substr(s1 + 1, s2 - s1 - 1);
                        if (!ts.empty()) fv.t = std::stoi(ts) - 1;
                        std::string ns = ft.substr(s2 + 1);
                        if (!ns.empty()) fv.n = std::stoi(ns) - 1;
                    }
                }
                face.push_back(fv);
            }
            if (face.size() >= 3)
                currentGroup().faces.push_back(face);
        }
    }

    // ----------------------------------------------------------------
    //  Строим MeshGroup для каждой группы
    // ----------------------------------------------------------------
    std::vector<MeshGroup> result;

    for (auto& grp : groups)
    {
        if (grp.faces.empty()) continue;

        MeshGroup mg;
        // заполнить материал
        auto it = allMats.find(grp.matName);
        if (it != allMats.end())
        {
            mg.material = it->second;
            // Добавляем dir-префикс к пути текстуры чтобы путь был
            // относительно рабочей директории (.exe), а не папки модели.
            // Например: dir="model/"  map_Kd="textures/tex.dds"
            //        -> diffuseTexture="model/textures/tex.dds"
            if (!mg.material.diffuseTexture.empty() && !dir.empty())
                mg.material.diffuseTexture = dir + mg.material.diffuseTexture;
        }
        else
        {
            mg.material.name = grp.matName;
            mg.material.Kd[0] = defaultR;
            mg.material.Kd[1] = defaultG;
            mg.material.Kd[2] = defaultB;
        }

        std::unordered_map<std::string, uint32_t> cache;

        auto getOrAdd = [&](const FaceVert& fv, const Vec3& flatN) -> uint32_t
            {
                std::string key = std::to_string(fv.p) + "/" +
                    std::to_string(fv.t) + "/" +
                    std::to_string(fv.n);
                auto ki = cache.find(key);
                if (ki != cache.end()) return ki->second;

                Vertex vert{};
                vert.pos[0] = positions[fv.p].x;
                vert.pos[1] = positions[fv.p].y;
                vert.pos[2] = positions[fv.p].z;

                if (fv.n >= 0 && fv.n < (int)normals.size())
                {
                    vert.normal[0] = normals[fv.n].x;
                    vert.normal[1] = normals[fv.n].y;
                    vert.normal[2] = normals[fv.n].z;
                }
                else
                {
                    vert.normal[0] = flatN.x;
                    vert.normal[1] = flatN.y;
                    vert.normal[2] = flatN.z;
                }

                if (fv.t >= 0 && fv.t < (int)texcoords.size())
                {
                    vert.uv[0] = texcoords[fv.t].u;
                    vert.uv[1] = texcoords[fv.t].v;
                }

                // Цвет из материала (используется если нет текстуры)
                vert.color[0] = mg.material.Kd[0];
                vert.color[1] = mg.material.Kd[1];
                vert.color[2] = mg.material.Kd[2];
                vert.color[3] = mg.material.d;

                uint32_t idx = (uint32_t)mg.mesh.vertices.size();
                mg.mesh.vertices.push_back(vert);
                cache[key] = idx;
                return idx;
            };

        for (auto& face : grp.faces)
        {
            // Плоская нормаль для граней без vn
            Vec3 flatN{ 0, 1, 0 };
            if (face.size() >= 3)
            {
                auto& p0 = positions[face[0].p];
                auto& p1 = positions[face[1].p];
                auto& p2 = positions[face[2].p];
                float ax = p1.x - p0.x, ay = p1.y - p0.y, az = p1.z - p0.z;
                float bx = p2.x - p0.x, by = p2.y - p0.y, bz = p2.z - p0.z;
                flatN.x = ay * bz - az * by;
                flatN.y = az * bx - ax * bz;
                flatN.z = ax * by - ay * bx;
                float len = sqrtf(flatN.x * flatN.x + flatN.y * flatN.y + flatN.z * flatN.z);
                if (len > 1e-6f) { flatN.x /= len; flatN.y /= len; flatN.z /= len; }
            }

            uint32_t i0 = getOrAdd(face[0], flatN);
            for (size_t i = 1; i + 1 < face.size(); ++i)
            {
                uint32_t i1 = getOrAdd(face[i], flatN);
                uint32_t i2 = getOrAdd(face[i + 1], flatN);
                mg.mesh.indices.push_back(i0);
                mg.mesh.indices.push_back(i1);
                mg.mesh.indices.push_back(i2);
            }
        }

        if (!mg.mesh.vertices.empty())
            result.push_back(std::move(mg));
    }

    if (result.empty())
        throw std::runtime_error("ObjLoader: no geometry found in: " + path);

    return result;
}