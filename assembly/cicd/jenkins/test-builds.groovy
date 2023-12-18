pipeline {
    agent none
    stages {
        stage('Run Builds') {
            parallel {
                stage('Ubuntu 20.04 x86-64 (shared)') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        /*
                        sudo apt-get update
                        sudo apt-get install -y build-essential git openssl cmake ninja-build zlib1g-dev libssl-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev
                        wget https://apt.llvm.org/llvm.sh
                        chmod +x llvm.sh
                        sudo ./llvm.sh 16 all
                        */
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-ubuntu-20.04-shared.sh .
                            chmod +x build-ubuntu-20.04-shared.sh
                            ./build-ubuntu-20.04-shared.sh -t -a
                            '''
                            sh 'zip -r ton-x86_64-linux-shared ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-x86_64-linux-shared.zip'
                        }
                    }
                }
                stage('Ubuntu 20.04 x86-64 (portable)') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-ubuntu-20.04-portable.sh .
                            chmod +x build-ubuntu-20.04-portable.sh
                            ./build-ubuntu-20.04-portable.sh -t -a
                            '''
                            sh 'zip -r ton-x86_64-linux-portable ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-x86_64-linux-portable.zip'
                        }
                    }
                }
                stage('Ubuntu 20.04 aarch64 (shared)') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-ubuntu-20.04-shared.sh .
                            chmod +x build-ubuntu-20.04-shared.sh
                            ./build-ubuntu-20.04-shared.sh -t -a
                            '''
                            sh 'zip -r ton-arm64-linux-shared ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-arm64-linux-shared.zip'
                        }
                    }
                }
                stage('Ubuntu 20.04 aarch64 (portable)') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-ubuntu-20.04-portable.sh .
                            chmod +x build-ubuntu-20.04-portable.sh
                            ./build-ubuntu-20.04-portable.sh -t -a
                            '''
                            sh 'zip -r ton-arm64-linux-portable ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-arm64-linux-portable.zip'
                        }
                    }
                }
                stage('macOS 12.7 x86-64 (shared)') {
                    agent {
                        label 'macOS_12.7_x86-64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-macos-shared.sh .
                            chmod +x build-macos-shared.sh
                            ./build-macos-shared.sh -t -a
                            '''
                            sh 'zip -r ton-x86-64-macos-shared ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-x86-64-macos-shared.zip'
                        }
                    }
                }
                stage('macOS 12.7 x86-64 (portable)') {
                    agent {
                        label 'macOS_12.7_x86-64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-macos-portable.sh .
                            chmod +x build-macos-portable.sh
                            ./build-macos-portable.sh -t -a -o 12.7
                            '''
                            sh 'zip -r ton-x86-64-macos-portable ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-x86-64-macos-portable.zip'
                        }
                    }
                }
                stage('macOS 12.6 aarch64 (shared)') {
                    agent {
                        label 'macOS_12.6.3-arm64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-macos-shared.sh .
                            chmod +x build-macos-shared.sh
                            ./build-macos-shared.sh -t -a
                            '''
                            sh 'zip -r ton-arm64-m1-macos-shared ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-arm64-m1-macos-shared.zip'
                        }
                    }
                }
                stage('macOS 12.6 aarch64 (portable)') {
                    agent {
                        label 'macOS_12.6.3-arm64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-macos-portable.sh .
                            chmod +x build-macos-portable.sh
                            ./build-macos-portable.sh -t -a -o 12.6
                            '''
                            sh 'zip -r ton-arm64-m1-macos-portable ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-arm64-m1-macos-portable.zip'
                        }
                    }
                }
                stage('macOS 13.2 aarch64 (shared)') {
                    agent {
                        label 'macOS_13.2-arm64-m2'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-macos-shared.sh .
                            chmod +x build-macos-shared.sh
                            ./build-macos-shared.sh -t -a
                            '''
                            sh 'zip -r ton-arm64-m2-macos-shared ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-arm64-m2-macos-shared.zip'
                        }
                    }
                }
                stage('macOS 13.2 aarch64 (portable)') {
                    agent {
                        label 'macOS_13.2-arm64-m2'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/native/build-macos-portable.sh .
                            chmod +x build-macos-portable.sh
                            ./build-macos-portable.sh -t -a -o 13.2
                            '''
                            sh 'zip -r ton-arm64-m2-macos-portable ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-arm64-m2-macos-portable.zip'
                        }
                    }
                }
                stage('Windows Server 2022 x86-64') {
                    agent {
                        label 'Windows_x86-64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            bat '''
                            copy assembly\\native\\build-windows-github.bat .
                            copy assembly\\native\\build-windows.bat .
                            build-windows-github.bat
                            '''
                            bat 'zip -r ton-x86-64-windows ./artifacts/*'
                            archiveArtifacts artifacts: 'ton-x86-64-windows.zip'
                        }
                    }
                }
                stage('Android Tonlib') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        timeout(time: 90, unit: 'MINUTES') {
                            sh '''
                            cp assembly/android/build-android-tonlib.sh .
                            chmod +x build-android-tonlib.sh
                            ./build-android-tonlib.sh -a
                            '''
                            sh 'zip -r ton-android-tonlib ./artifacts/tonlib-android-jni/*'
                            archiveArtifacts artifacts: 'ton-android-tonlib'
                        }
                    }
                }
            }
        }
    }
}