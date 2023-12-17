pipeline {
    agent none
    stages {
        stage('Run Builds') {
            parallel {
                stage('Ubuntu 20.04 x86-64') {
                    agent {
                        label 'Ubuntu_x86-64'
                    }
                    steps {
                        sh 'pwd; ls; lsb_release -a; touch a; git branch'
                    }
                    post {
                        always {
                            sh 'echo ok 1'
                            archiveArtifacts artifacts: 'artifacts/*.*'
                        }
                    }
                }
                stage('Ubuntu 20.04 arm64') {
                    agent {
                        label 'Ubuntu_arm64'
                    }
                    steps {
                        sh 'pwd; ls; lsb_release -a; touch b; git branch'
                    }
                    post {
                        always {
                            sh 'echo ok 2'
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
                            sh 'echo ok 3'
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
                            sh 'echo ok 4'
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
                            sh 'echo ok 5'
                        }
                    }
                }
                stage('Windows Server 2022 x86-64') {
                    agent {
                        label 'Windows_x86-64'
                    }
                    steps {
                        sh 'echo %cd%'
                        sh 'git branch'
                    }
                    post {
                        always {
                            sh 'echo ok 6'
                        }
                    }
                }
            }
        }
    }
}