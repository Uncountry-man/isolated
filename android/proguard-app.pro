# Regras do app para o R8 (build.sh). As das bibliotecas vêm de tools/collect_proguard.py e as do manifest do aapt2.
# Todo o Java do jogo é chamado pelo C++ via JNI (GetStaticMethodID/FindClass por nome): manter tudo, sem renomear.
-keep class com.rungo.isolatedMT.** { *; }
# Sem ofuscar: nomes reais nos crashes e nada de reflexão/ServiceLoader quebrando por nome trocado (o ganho vem do encolhimento).
-dontobfuscate
-keepattributes *Annotation*,Signature,InnerClasses,EnclosingMethod,SourceFile,LineNumberTable
# Classes opcionais referenciadas pelas bibliotecas mas ausentes do classpath (Kotlin/Java SE/anotações de compilação)
-dontwarn **
-ignorewarnings
