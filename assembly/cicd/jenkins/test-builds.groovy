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
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/native/build-ubuntu-shared.sh .
                                chmod +x build-ubuntu-shared.sh
                                ./build-ubuntu-shared.sh -t -a
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-x86_64-linux-shared ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-x86_64-linux-shared.zip'
                        }
                    }
                }
                stage('Ubuntu 20.04 x86-64 (portable)') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/nix/build-linux-x86-64-nix.sh .
                                chmod +x build-linux-x86-64-nix.sh
                                ./build-linux-x86-64-nix.sh -t
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-x86-64-linux-portable ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-x86-64-linux-portable.zip'
                        }
                    }
                }
                stage('Ubuntu 20.04 aarch64 (shared)') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/native/build-ubuntu-shared.sh .
                                chmod +x build-ubuntu-shared.sh
                                ./build-ubuntu-shared.sh -t -a
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-arm64-linux-shared ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-arm64-linux-shared.zip'
                        }
                    }
                }
                stage('Ubuntu 20.04 aarch64 (portable)') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/nix/build-linux-arm64-nix.sh .
                                chmod +x build-linux-arm64-nix.sh
                                ./build-linux-arm64-nix.sh -t
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-arm64-linux-portable ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-arm64-linux-portable.zip'
                        }
                    }
                }
                stage('macOS 12.7 x86-64 (shared)') {
                    agent {
                        label 'macOS_12.7_x86-64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/native/build-macos-shared.sh .
                                chmod +x build-macos-shared.sh
                                ./build-macos-shared.sh -t -a
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-x86-64-macos-shared ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-x86-64-macos-shared.zip'
                        }
                    }
                }
                stage('macOS 12.7 x86-64 (portable)') {
                    agent {
                        label 'macOS_12.7_x86-64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/nix/build-macos-nix.sh .
                                chmod +x build-macos-nix.sh
                                ./build-macos-nix.sh -t
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-x86-64-macos-portable ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-x86-64-macos-portable.zip'
                        }
                    }
                }
                stage('macOS 12.6 aarch64 (shared)') {
                    agent {
                        label 'macOS_12.6-arm64-m1'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/native/build-macos-shared.sh .
                                chmod +x build-macos-shared.sh
                                ./build-macos-shared.sh -t -a
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-arm64-macos-m1-shared ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-arm64-macos-m1-shared.zip'
                        }
                    }
                }
                stage('macOS 12.6 aarch64 (portable)') {
                    agent {
                        label 'macOS_12.6-arm64-m1'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/nix/build-macos-nix.sh .
                                chmod +x build-macos-nix.sh
                                ./build-macos-nix.sh -t
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-arm64-macos-portable ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-arm64-macos-portable.zip'
                        }
                    }
                }
                stage('macOS 13.2 aarch64 (shared)') {
                    agent {
                        label 'macOS_13.2-arm64-m2'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/native/build-macos-shared.sh .
                                chmod +x build-macos-shared.sh
                                ./build-macos-shared.sh -t -a
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-arm64-macos-m2-shared ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-arm64-macos-m2-shared.zip'
                        }
                    }
                }
                stage('Windows Server 2022 x86-64') {
                    agent {
                        label 'Windows_x86-64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            bat '''
                                copy assembly\\native\\build-windows.bat .
                                build-windows.bat -t
                            '''
                            bat '''
                                cd artifacts
                                zip -9r ton-x86-64-windows ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-x86-64-windows.zip'
                        }
                    }
                }
                stage('Android Tonlib') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cp assembly/android/build-android-tonlib.sh .
                                chmod +x build-android-tonlib.sh
                                ./build-android-tonlib.sh -a
                            '''
                            sh '''
                                cd artifacts/tonlib-android-jni
                                zip -9r ton-android-tonlib ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/tonlib-android-jni/ton-android-tonlib.zip'
                        }
                    }
                }
                stage('WASM fift func emulator') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        timeout(time: 180, unit: 'MINUTES') {
                            sh '''
                                cd assembly/wasm
                                chmod +x fift-func-wasm-build-ubuntu.sh
                                ./fift-func-wasm-build-ubuntu.sh -a
                            '''
                            sh '''
                                cd artifacts
                                zip -9r ton-wasm-binaries ./*
                            '''
                            archiveArtifacts artifacts: 'artifacts/ton-wasm-binaries.zip'
                        }
                    }
                }
            }
        }
    }
}
