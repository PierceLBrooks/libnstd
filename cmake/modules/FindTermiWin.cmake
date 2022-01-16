include(FindPackageHandleStandardArgs)

if (WIN32)

    find_path(TERMIWIN_INCLUDE_DIR
        NAMES
            termios.h
        HINTS
            "${TERMIWIN_LOCATION}/include"
            "$ENV{TERMIWIN_LOCATION}/include"
        PATHS
            "$ENV{PROGRAMFILES}/termiWin/include"
        DOC "The directory where termios.h resides" )

    find_library(TERMIWIN_LIBRARY
        NAMES
            TERMIWIN termiWin termiwin TermiWin
        HINTS
            "${TERMIWIN_LOCATION}/lib"
            "$ENV{TERMIWIN_LOCATION}/lib"
        PATHS
            "$ENV{PROGRAMFILES}/termiWin/lib"
        DOC "The TermiWin library")
endif ()

if (${CMAKE_HOST_UNIX})
    find_path( TERMIWIN_INCLUDE_DIR
        NAMES
            termios.h
        HINTS
            "${TERMIWIN_LOCATION}/include"
            "$ENV{TERMIWIN_LOCATION}/include"
        PATHS
            /usr/include
            /usr/local/include
            /sw/include
            /opt/local/include
            NO_DEFAULT_PATH
            DOC "The directory where termios.h resides"
    )
    find_library( TERMIWIN_LIBRARY
        NAMES
            TERMIWIN termiWin termiwin TermiWin
        HINTS
            "${TERMIWIN_LOCATION}/lib"
            "$ENV{TERMIWIN_LOCATION}/lib"
        PATHS
            /usr/lib64
            /usr/lib
            /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
            /usr/local/lib64
            /usr/local/lib
            /sw/lib
            /opt/local/lib
            NO_DEFAULT_PATH
            DOC "The TermiWin library")
endif ()

find_package_handle_standard_args(TermiWin
    REQUIRED_VARS
        TERMIWIN_INCLUDE_DIR
        TERMIWIN_LIBRARY
)
