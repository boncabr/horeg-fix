#!/bin/sh
# Gradle start-up script for UN*X
# dlms losss — mas ari

APP_NAME="Gradle"
APP_BASE_NAME=`basename "$0"`

SAVED="`pwd`"
cd "`dirname \"$0\"`/" >/dev/null
APP_HOME="`pwd -P`"
cd "$SAVED" >/dev/null

CLASSPATH=$APP_HOME/gradle/wrapper/gradle-wrapper.jar

if [ -n "$JAVA_HOME" ] ; then
    JAVACMD="$JAVA_HOME/bin/java"
else
    JAVACMD="java"
fi

exec "$JAVACMD" \
  -Xmx64m \
  -Xms64m \
  -classpath "$CLASSPATH" \
  org.gradle.wrapper.GradleWrapperMain \
  "$@"
