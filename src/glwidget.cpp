#include "glwidget.h"
#include "resourceloader.h"
#include "errorchecker.h"
#include "hairCommon.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include "hairobject.h"
#include "simulation.h"
#include "hairshaderprogram.h"
#include "meshshaderprogram.h"
#include "hairopacityshaderprogram.h"
#include "hairinterface.h"
#include "texture.h"
#include "framebuffer.h"

GLWidget::GLWidget(QGLFormat format, HairInterface *hairInterface, QWidget *parent)
    : QGLWidget(format, parent),
      m_hairInterface(hairInterface),
      m_hairDensity(40),
      m_timer(this),
      m_increment(0),
      m_targetFPS(60.f)
{
    m_highResMesh = NULL;
    m_lowResMesh = NULL;
    m_hairObject = NULL;
    m_testSimulation = NULL;
    m_hairProgram = new HairShaderProgram();
    m_meshProgram = new MeshShaderProgram();
    m_opacityMapProgram = new HairOpacityShaderProgram();
    m_noiseTexture = new Texture();
    m_shadowDepthTexture = new Texture();
    m_opacityMapTexture = new Texture();
    m_shadowFramebuffer = new Framebuffer();
    m_opacityMapFramebuffer = new Framebuffer();
    m_hairInterface->setGLWidget(this);

    // Set up 60 FPS draw loop.
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(updateCanvas()));
    m_timer.start(1000.0f / m_targetFPS);
}

GLWidget::~GLWidget()
{
    safeDelete(m_highResMesh);
    safeDelete(m_lowResMesh);
    safeDelete(m_testSimulation);
    safeDelete(m_hairObject);
    safeDelete(m_hairProgram);
    safeDelete(m_meshProgram);
    safeDelete(m_opacityMapTexture);
    safeDelete(m_opacityMapFramebuffer);
    safeDelete(m_noiseTexture);
}

void GLWidget::initializeGL()
{
    ResourceLoader::initializeGlew();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glClearColor(0.5f, 0.5f, 0.5f, 0.0f);

    // Initialize shader programs.
    m_hairProgram->create();
    m_meshProgram->create();
    m_opacityMapProgram->create();

    // Initialize textures.
    m_noiseTexture->create(":/images/noise128.jpg", GL_LINEAR, GL_LINEAR);
    m_shadowDepthTexture->createDepthTexture(1024, 1024);
    m_opacityMapTexture->createColorTexture(1024, 1024, GL_NEAREST, GL_NEAREST);

    // Initialize framebuffers.
    m_shadowFramebuffer->create();
    m_shadowFramebuffer->attachDepthTexture(m_shadowDepthTexture->id);
    m_opacityMapFramebuffer->create();
    std::vector<GLuint> textures { m_opacityMapTexture->id };
    m_opacityMapFramebuffer->attachColorTextures(textures);
    m_opacityMapFramebuffer->generateDepthBuffer(1024, 1024);
    
    // Initialize simulation.
    initSimulation();

    // Initialize global view and projection matrices.
    m_view = glm::lookAt(glm::vec3(0,0,m_zoom), glm::vec3(0), glm::vec3(0,1,0));
    m_projection = glm::perspective(0.8f, (float)width()/height(), 0.1f, 100.f);
    
    ErrorChecker::printGLErrors("end of initializeGL");
}

void GLWidget::paintGL()
{
    ErrorChecker::printGLErrors("start of paintGL");

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float time = m_increment++ / (float) m_targetFPS;      // Time in seconds.

    m_testSimulation->update(time);
    m_hairObject->update(time);

    glm::mat4 model = glm::mat4(1.f);
    glm::vec3 lightPosition = glm::vec3(2, 1, 3);
    glm::mat4 lightProjection = glm::perspective(1.3f, 1.f, .1f, 100.f);
    glm::mat4 lightView = glm::lookAt(lightPosition, glm::vec3(0), glm::vec3(0,1,0));
    glm::mat4 eyeToLight = lightProjection * lightView * glm::inverse(m_view);

    // Bind textures.
    m_noiseTexture->bind(GL_TEXTURE0);
    m_shadowDepthTexture->bind(GL_TEXTURE1);
    m_opacityMapTexture->bind(GL_TEXTURE2);

    ShaderProgram *program;

    // Render shadow map.
    glViewport(0, 0, m_shadowDepthTexture->width(), m_shadowDepthTexture->height());
    m_shadowFramebuffer->bind();
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        program = m_meshProgram;
        {
            program->bind();
            program->uniforms.projection = lightProjection;
            program->uniforms.view = lightView;
            program->uniforms.model = model;
            program->setGlobalUniforms();
            program->setPerObjectUniforms();
            m_highResMesh->draw();
        }

        program = m_hairProgram;
        {
            program->bind();
            program->uniforms.noiseTexture = 0;
            program->uniforms.shadowMap = 1;
            program->uniforms.projection = lightProjection;
            program->uniforms.view = lightView;
            program->uniforms.model = model;
            program->uniforms.eyeToLight = eyeToLight;
            program->uniforms.lightPosition = lightPosition;
            program->setGlobalUniforms();
            m_hairObject->paint(program);
        }
    }
    m_shadowFramebuffer->unbind();

    // Render opacity map.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glViewport(0, 0, m_shadowDepthTexture->width(), m_shadowDepthTexture->height());
    m_opacityMapFramebuffer->bind();
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        program = m_opacityMapProgram;
        {
            program->bind();
            program->uniforms.noiseTexture = 0;
            program->uniforms.shadowMap = 1;
            program->uniforms.projection = lightProjection;
            program->uniforms.view = lightView;
            program->uniforms.model = model;
            program->uniforms.eyeToLight = eyeToLight;
            program->uniforms.lightPosition = lightPosition;
            program->setGlobalUniforms();
            m_hairObject->paint(program);
        }
    }
    m_opacityMapFramebuffer->unbind();
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Render hair.
    glViewport(0, 0, width(), height());
    program = m_hairProgram;
    {
        program->bind();
        program->uniforms.noiseTexture = 0;
        program->uniforms.shadowMap = 1;
        program->uniforms.opacityMap = 2;
        program->uniforms.projection = m_projection;
        program->uniforms.view = m_view;
        program->uniforms.model = model;
        program->uniforms.eyeToLight = eyeToLight;
        program->uniforms.lightPosition = lightPosition;
        program->uniforms.shadowIntensity = 1.5;
        program->setGlobalUniforms();
        m_hairObject->paint(program);
    }
    
    // Render mesh.
    program = m_meshProgram;
    {
        program->bind();
        program->uniforms.shadowMap = 1;
        program->uniforms.opacityMap = 2;
        program->uniforms.projection = m_projection;
        program->uniforms.view = m_view;
        program->uniforms.model = model;
        program->uniforms.lightPosition = lightPosition;
        program->uniforms.eyeToLight = eyeToLight;
        program->uniforms.shadowIntensity = 0.8;
        program->setGlobalUniforms();
        program->setPerObjectUniforms();
        m_highResMesh->draw();
    }

    // Clean up.
    program->unbind();
    m_opacityMapTexture->unbind(GL_TEXTURE2);
    m_shadowDepthTexture->unbind(GL_TEXTURE1);
    m_noiseTexture->unbind(GL_TEXTURE0);

    // Update UI.
    m_hairInterface->updateFPSLabel(m_increment);

}


void GLWidget::initSimulation()
{
    safeDelete(m_highResMesh);
    safeDelete(m_lowResMesh);
    safeDelete(m_testSimulation);
    HairObject *_oldHairObject = m_hairObject;

    m_highResMesh = new ObjMesh();
    m_highResMesh->init(":/models/head.obj");

    m_lowResMesh = new ObjMesh();
    m_lowResMesh->init(":/models/headLowRes.obj", 1.1);

    m_testSimulation = new Simulation(m_lowResMesh);
    m_hairObject = new HairObject(
                m_highResMesh, m_hairDensity, ":/images/headHair.jpg", m_testSimulation, m_hairObject);

    safeDelete(_oldHairObject);

    m_hairInterface->setHairObject(m_hairObject);
}


void GLWidget::resetSimulation()
{

    initSimulation();
}


void GLWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
    m_projection = glm::perspective(0.8f, (float)width()/height(), 0.1f, 100.f);
}

/** Repaints the canvas. Called 60 times per second. */
void GLWidget::updateCanvas()
{
    update();
}

void GLWidget::mousePressEvent(QMouseEvent *event)
{
    m_prevMousePos = event->pos();
}

void GLWidget::mouseMoveEvent(QMouseEvent *event)
{
    m_angleX += 10 * (event->x() - m_prevMousePos.x()) / (float) width();
    m_angleY += 10 * (event->y() - m_prevMousePos.y()) / (float) height();
    m_view = glm::translate(glm::vec3(0, 0, -m_zoom)) *
            glm::rotate(m_angleY, glm::vec3(1, 0, 0)) *
            glm::rotate(m_angleX, glm::vec3(0, 1, 0));
    m_prevMousePos = event->pos();
}

void GLWidget::wheelEvent(QWheelEvent *event)
{
    m_zoom -= event->delta() / 100.f;
    m_view = glm::translate(glm::vec3(0, 0, -m_zoom)) *
            glm::rotate(m_angleY, glm::vec3(1, 0, 0)) *
            glm::rotate(m_angleX, glm::vec3(0, 1, 0));
}
