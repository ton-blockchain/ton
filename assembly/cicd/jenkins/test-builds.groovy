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
                        sh '''
                            cp assembly/native/build-ubuntu-20.04-shared.sh .
                            chmod +x build-ubuntu-20.04-shared.sh
                            ./build-ubuntu-20.04-shared.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('Ubuntu 20.04 x86-64 (portable)') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-ubuntu-20.04-portable.sh .
                            chmod +x build-ubuntu-20.04-portable.sh
                            ./build-ubuntu-20.04-portable.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('Ubuntu 20.04 arm64 (shared)') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-ubuntu-20.04-shared.sh .
                            chmod +x build-ubuntu-20.04-shared.sh
                            ./build-ubuntu-20.04-shared.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('Ubuntu 20.04 arm64 (portable)') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-ubuntu-20.04-portable.sh .
                            chmod +x build-ubuntu-20.04-portable.sh
                            ./build-ubuntu-20.04-portable.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('macOS 12.7 x86-64 (shared)') {
                    agent {
                        label 'macOS_12.7_x86-64'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-macos-shared.sh .
                            chmod +x build-macos-shared.sh
                            ./build-macos-shared.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('macOS 12.7 x86-64 (portable)') {
                    agent {
                        label 'macOS_12.7_x86-64'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-macos-portable.sh .
                            chmod +x build-macos-portable.sh
                            ./build-macos-portable.sh -t -a -o 12.7
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('macOS 12.6 arm64 (M1, shared)') {
                    agent {
                        label 'macOS_12.6.3-arm64'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-macos-shared.sh .
                            chmod +x build-macos-shared.sh
                            ./build-macos-shared.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('macOS 12.6 arm64 (M1, portable)') {
                    agent {
                        label 'macOS_12.6.3-arm64'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-macos-portable.sh .
                            chmod +x build-macos-portable.sh
                            ./build-macos-portable.sh -t -a -o 12.6
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('macOS 13.2 arm64 (M2, shared)') {
                    agent {
                        label 'macOS_13.2-arm64-m2'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-macos-shared.sh .
                            chmod +x build-macos-shared.sh
                            ./build-macos-shared.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('macOS 13.2 arm64 (M2, portable)') {
                    agent {
                        label 'macOS_13.2-arm64-m2'
                    }
                    steps {
                        sh '''
                            cp assembly/native/build-macos-portable.sh .
                            chmod +x build-macos-portable.sh
                            ./build-macos-portable.sh -t -a -o 13.2
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/*'
                        }
                    }
                }
                stage('Windows Server 2022 x86-64') {
                    agent {
                        label 'Windows_x86-64'
                    }
                    steps {
                        bat '''
                            copy assembly\\native\\build-windows-github.bat .
                            copy assembly\\native\\build-windows.bat .
                            build-windows-github.bat
                            '''
                        bat 'git branch'
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts\\*'
                        }
                    }
                }
            }
        }
    }
}