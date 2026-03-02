#include "../../data_win.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <stdio.h>

#include "../../utils.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path to data.win or game.unx>\n", argv[0]);
        return 1;
    }

    const char* filePath = argv[1];
    printf("Loading %s...\n", filePath);

    DataWin* dataWin = DataWin_parse(filePath);

    Gen8* gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully!\n", gen8->name, gen8->gameID);

    // Build window title
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

    // Init GLFW
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        DataWin_free(dataWin);
        return 1;
    }

    GLFWwindow* window = glfwCreateWindow((int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight, windowTitle, nullptr, nullptr);
    if (window == nullptr) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        DataWin_free(dataWin);
        return 1;
    }

    glfwMakeContextCurrent(window);

    // Load OpenGL function pointers via GLAD
    if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) {
        fprintf(stderr, "Failed to initialize GLAD\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        DataWin_free(dataWin);
        return 1;
    }

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    glfwDestroyWindow(window);
    glfwTerminate();
    DataWin_free(dataWin);
    return 0;
}
