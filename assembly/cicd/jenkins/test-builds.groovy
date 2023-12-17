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
                        sh '''sudo apt-get update
                            sudo apt-get install -y build-essential git openssl cmake ninja-build zlib1g-dev libssl-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev
                            wget https://apt.llvm.org/llvm.sh
                            chmod +x llvm.sh
                            sudo ./llvm.sh 16 all
                            cp assembly/native/build-ubuntu-20.04-shared.sh .
                            chmod +x build-ubuntu-20.04-shared.sh
                            ./build-ubuntu-20.04-shared.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/**/*.*'
                        }
                    }
                }
                stage('Ubuntu 20.04 arm64 (shared)') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        sh '''sudo apt-get update
                            sudo apt-get install -y build-essential git openssl cmake ninja-build zlib1g-dev libssl-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev
                            wget https://apt.llvm.org/llvm.sh
                            chmod +x llvm.sh
                            sudo ./llvm.sh 16 all
                            cp assembly/native/build-ubuntu-20.04-shared.sh .
                            chmod +x build-ubuntu-20.04-shared.sh
                            ./build-ubuntu-20.04-shared.sh -t -a
                           '''
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/**/*.*'
                        }
                    }
                }
                stage('macOS 12.7 x86-64') {
                    agent {
                        label 'macOS_12.7_x86-64'
                    }
                    steps {
                        sh 'pwd; ls; sw_vers; touch c; git branch'
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/**/*.*'
                        }
                    }
                }
                stage('macOS 12.6 arm64 (M1)') {
                    agent {
                        label 'macOS_12.6.3-arm64'
                    }
                    steps {
                        sh 'pwd; ls; sw_vers; touch 4; git branch'
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/**/*.*'
                        }
                    }
                }
                stage('macOS 13.2 arm64 (M2)') {
                    agent {
                        label 'macOS_13.2-arm64-m2'
                    }
                    steps {
                        sh 'pwd; ls; sw_vers; touch e; git branch'
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/**/*.*'
                        }
                    }
                }
                stage('Windows Server 2022 x86-64') {
                    agent {
                        label 'Windows_x86-64'
                    }
                    steps {
                        bat 'echo %cd%'
                        bat 'git branch'
                    }
                    post {
                        always {
                            archiveArtifacts artifacts: 'artifacts/**/*.*'
                        }
                    }
                }
            }
        }
    }
}