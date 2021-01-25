OpenR: enrutamiento abierto
Estado de construcción Estado de la documentación

Open Routing, OpenR, es el protocolo / plataforma de enrutamiento interior diseñado y desarrollado internamente por Facebook. OpenR fue originalmente diseñado y construido para realizar enrutamiento en la red de malla Terragraph . El diseño flexible de OpenR ha llevado a su adopción en otras redes, incluida la nueva red WAN de Facebook, Express Backbone.

Documentación
Consulte nuestra extensa documentación para comenzar con OpenR.

Ejemplos de biblioteca
Consulte el examplesdirectorio para ver algunas formas útiles de aprovechar las bibliotecas openr y fbzmq para crear software que se ejecute con OpenR.

Recursos
Grupo de desarrolladores: https://www.facebook.com/groups/openr/
Github: https://github.com/facebook/openr/
IRC: #openr en freenode
Contribuir
Eche un vistazo Developer Guide y CONTRIBUTING.mdcomience a contribuir. La Guía para desarrolladores describe las mejores prácticas para la contribución y las pruebas de código. Cualquier cambio individual debe probarse bien para detectar regresiones y compatibilidad de versiones.

Código de Conducta
El código de conducta se describe en CODE_OF_CONDUCT.md

Requisitos
Lo hemos probado OpenRen Ubuntu-16.04, Ubuntu-18.04 y CentOS 7/8. OpenR debería funcionar en todas las plataformas basadas en Linux.

Compilador compatible con C ++ 17 o superior
libzmq-4.0.6 o superior
Construir
Estructura del directorio de repositorios
En el nivel superior de este repositorio se encuentran los directorios buildy openr. Debajo del primero hay una herramienta gen, que contiene scripts para construir el proyecto. El openrdirectorio contiene la fuente del proyecto.

Dependencias
OpenR requiere estas dependencias para su sistema y sigue los pasos tradicionales de compilación de cmake a continuación.

cmake
gflags
gtest
libsodium
libzmq
zstd
folly
fbthrift
fbzmq
re2-devel
Compilación en un paso: Ubuntu
Hemos proporcionado un script build/build_openr.sh, probado en versiones de Ubuntu LTS. Se utiliza gendeps.pypara instalar todas las dependencias necesarias, compilar OpenR e instalar binarios de C ++, así como herramientas de Python. Modifique el script según sea necesario para su plataforma. Además, tenga en cuenta que algunas dependencias de la biblioteca requieren una versión más reciente que la proporcionada por el administrador de paquetes predeterminado en el sistema y, por lo tanto, las estamos compilando desde la fuente en lugar de instalarlas a través del administrador de paquetes. Consulte el script para esas instancias y las versiones necesarias.

Pasos de construcción
# Instalar dependencias y openr 
cd build 
bash ./build_openr.sh

# Para ejecutar pruebas (algunas pruebas requieren privilegios de sudo) 
python3 build / fbcode_builder / getdeps.py test \ 
  --src-dir =. \ 
  --project-install-prefix openr: / opt / facebook \ 
  openr
Si realiza algún cambio, puede ejecutar cmake ../openry makedesde el directorio de compilación para compilar openr con sus cambios.

Instalando
openrconstruye tanto todos los archivos de cabecera a las bibliotecas estáticas y dinámicas y las bibliotecas de instalación instala paso y de /opt/facebook/openr/liby /opt/facebook/openr/include/junto con los módulos de Python en Python de su site-packagesdirectorio. Nota: el build_openr.shscript ejecutará este paso por usted

Puede conducir manualmente getdeps.pypara instalar en otro lugar
Referirse a build_openr.sh
Instalación de bibliotecas de Python
Necesitará python pipo setuptoolscrear e instalar módulos de python. Todas las dependencias de la biblioteca se instalarán automáticamente excepto el fbthrift-pythonmódulo que deberá instalar manualmente siguiendo pasos similares a los que se describen a continuación. Esto instalará breezeuna herramienta cli para interactuar con OpenR.

La instalación de Python requiere que se instale un compilador fbthrift/ thrift1y en PATH
cd openr / openr / py 
python setup.py build 
sudo python setup.py install
Construcción / uso de Docker
OpenR ahora tiene un Dockerfile. Se usa gendeps.pypara construir todas las dependencias + OpenR. También instala OpenR CLI breezeen el contenedor.

 docker build: host de red.
Corriendo
Puede especificar un archivo de configuración vinculando el montaje de un directorio con un openr.cfgarchivo en / config

Docker ejecutar --name openr - host de red openr_ubuntu
Para usar un montaje de enlace de configuración personalizado /configen el contenedor
El binario OpenR buscará /config/openr.conf
Licencia
OpenR tiene licencia del MIT .
